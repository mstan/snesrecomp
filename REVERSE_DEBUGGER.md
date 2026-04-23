# Reverse Debugger for snesrecomp

A build-flag-gated stepper + reverse debugger for statically recompiled
SNES code. Not an emulator instrument: the recompiler itself emits the
observability, so what we see is the exact C the generator produces, at
C-block granularity.

## North star

Make every bug that's currently "wait why does recomp diverge from oracle"
solvable in one session by: watching every observable event, pausing at
any point, stepping forward or back a block at a time, and inspecting
state mid-function without rebuilding. Mid-loop, mid-dispatch, whatever.

## Non-negotiables

- **Opt-in at compile time.** When the flag is off, ZERO code — no stubs,
  no comments, no `if (0)`, no empty function calls. The `recomp.py`
  generator emits a different body entirely based on the flag. A non-debug
  build is byte-for-byte what it is today.
- **Slowdown is expected when on.** No budget for "fast enough for
  production." Debug mode is for bisecting, not for playing.
- **In-memory only.** No files, no sockets as the hot path. Logs are
  ring buffers in the binary, dumped on TCP request.
- **Determinism is the axis everything else leans on.** Static recomp
  is already deterministic given (ROM, WRAM-in, controller input). The
  log just records the sequence; replay = re-run.

## Tier layout

We build tiers in order. Each tier is a standalone tool we exercise
against an actual open bug as its litmus test. If a tier closes the
current bug, we stop. If it doesn't, we build the next one.

### Tier 1 — synchronous WRAM write hook

**What it does.** Every generated `g_ram[x] = …` and every
`*(uint16 *)(g_ram + x) = …` becomes a call to
`debug_on_wram_write_byte(addr, old, new)` or
`debug_on_wram_write_word(addr, old, new)`. The inline RDB_STORE8/16
helpers read `g_ram[addr]` before the store to capture `old`, then
store, then call the hook; `func` / `block_id` come from the
`g_last_recomp_func` / `g_block_counter` globals inside the hook.
Every hook call
records to an in-memory ring buffer (capped at ~1M events), filtered
by a TCP-configurable address range so we don't drown.

**What it costs.** Every store becomes a function call with 4 args plus
a range check. Tier 1 builds are ~20-40% slower.

**What it wins.** No more polling blind spots. Every byte written to
$1C3E gets logged with `(frame, addr, old, new, function, block_label)`.
The current ground bug at $1C3E becomes "dump the write list to that
address and find the first divergent value" — hours, not days.

**Litmus test.** Run with Tier 1 on against
`_probe_invoke_forced.py FFF8 2 17` and dump every write to
$1BE6-$1CE5 during the invoke. Compare recomp's written values at
iter 22 to what oracle would produce. If the first divergent value is
in `BufferScrollingTiles_Layer1`'s inner loop, the tier closed the bug.

### Tier 1.5 — call trace (shipped)

**What it does.** Per-`RecompStackPush` ring buffer, 65k entries.
Each entry records `(frame, depth, func, parent)` at the moment of
the call. Hooked into `debug_server_profile_push`, which is invoked
from `RecompStackPush` unconditionally when `SNESRECOMP_REVERSE_DEBUG`
is on.

TCP:
- `trace_calls` / `trace_calls_reset` — arm / reset the ring.
- `get_call_trace` with filters `from`, `to`, `max_depth`, `contains`.

**Caveat.** `RECOMP_STACK_DEPTH` caps at 16; beyond that, parent /
depth fields become unreliable. Prefer Tier 2's `pc_lo`/`pc_hi`
filter for attribution past that depth.

### Tier 2 — block-level trace (shipped)

**What it does.** `recomp.py --reverse-debug` emits
`RDB_BLOCK_HOOK(pc)` at every basic-block boundary — function entry
and every `label_xxxx:;`. The hook writes to a 256k-entry ring with
`(frame, depth, pc, func)`. Bank 0 alone gets ~1362 hooks; ~104k
blocks fire per attract-demo frame.

TCP:
- `trace_blocks` / `trace_blocks_reset` — arm / reset the ring.
- `get_block_trace` with filters `from`, `to`, `func`, `pc_lo`, `pc_hi`.

### Tier 2.5 — pause-on-block + WRAM watchpoints (shipped)

**What it does.** Two pause primitives share the main thread's
existing `s_paused` spin loop (`debug_server_wait_if_paused`).

*Block breakpoints.* `s_rdb_break_pcs[16]` holds armed PCs.
`debug_on_block_enter` checks `s_rdb_break_armed` (volatile-int fast
path, almost always falls through); on hit, parks the main thread
and exposes the parked PC via the `parked` command.

*WRAM watchpoints.* `s_rdb_watches[16]` holds `(addr, match_val)`
entries. `debug_on_wram_write_byte/word` check `s_rdb_watch_armed`
after the trace record; on hit, park the main thread. Watch matches
on exact address — word writes also match watches at `addr+1`. An
optional `match_val` restricts the watch to one specific written
value (e.g., "pause when $72 is written with 0x00"). The `parked`
command reports `watch_addr`, `watch_val`, `watch_width`, and the
writing function (captured from `g_last_recomp_func`).

TCP:
- `break_add <hex_pc>` / `break_clear` / `break_list` / `break_continue`
- `step_block` — arm a one-shot pause at the very next block hook
- `watch_add <hex_addr> [hex_val]` / `watch_clear` / `watch_list`
  / `watch_continue`
- `parked` — unified "why / where parked" report.

Pause happens *after* trace recording, so the trace still captures
the parked event. Continue commands just clear `s_paused`; any
other armed breakpoints / watchpoints stay live for the next hit.

**What it costs.** One flag test per block (~ns in the fast path). When
paused, real wall time. When a breakpoint fires, the spin blocks the
thread doing game execution entirely.

**What it wins.** The recomp binary is now a debugger target. You can
break at the label before a suspected bad computation, step block-by-block
to watch v27 / v30 / Map16LowPtr evolve, and `get_state` at each stop.

**Litmus test.** Break at `BufferScrollingTiles_Layer1:label_8a55` on
outer-loop iter 32. Step blocks until v33 is computed. Dump
`(v27, v30, Map16LowPtr, v31)` on both sides via paired recomp-vs-oracle
sessions (if oracle gets a parallel Tier 2), or vs. a known-good manual
computation.

### Tier 3 — reverse stepping (time travel)

**What it does.** Tier 2's block checkpoints become recording points.
At each block entry, we log `(block_id, WRAM delta since last block)`
to an in-memory ring. Periodic full WRAM snapshots (every N blocks)
anchor the log so we can rewind.

New server commands:

- `step_back` — roll execution back one block. Implementation: find the
  nearest prior snapshot, replay forward to the target block minus one.
- `run_back_until <condition>` — rewind until predicate is true.
- `timeline <cursor>` — dump the surrounding N blocks' history.

**What it costs.** Log + snapshots eat RAM. 10 min of 60 Hz gameplay
at ~100 blocks per frame = ~3.6M blocks. At ~8 B per block event
plus snapshots every 10k blocks (~130 KB each) → ~30-60 MB/min. We cap
at ~500 MB total (a few minutes of rewind history).

**What it wins.** "Why does iter 32 blank out? Step back to iter 22
and compare v30." Real reverse debugging, deterministic replay, no
hardware or kernel tricks needed since the recomp is pure C.

**Litmus test.** From a paused state at iter 32 showing blank output,
`step_back 50 blocks` → inspect state at iter 22 → `step_back more`
until we reach the write where Map16LowPtr diverged from expected.
If that write is visible in the reverse timeline, Tier 3 works.

### Tier 4 — C-line granularity (deferred)

**What it does.** `recomp.py` emits a checkpoint between every
C statement instead of just at labels. The log grows ~10x.

**Why deferred.** Tier 2's block granularity maps to SMWDisX basic
blocks, which is ~1-10 C lines each. That's already fine-grained enough
for almost every bug. Build this only if we hit something Tier 3
can't resolve.

## Build flag design

`snesrecomp/runner/src/debug_server.h`:

```c
#ifndef SNESRECOMP_REVERSE_DEBUG
#define SNESRECOMP_REVERSE_DEBUG 0
#endif
```

`recomp.py` takes `--reverse-debug` and emits one of two code paths
for every store and every block entry. **There is no runtime branch
in the store-emitting case** — the generator picks which C to write
based on the flag, and a non-debug regen produces byte-for-byte
identical `src/gen/*.c` to the current baseline.

Build system: the flag is a preprocessor define passed through MSBuild
`<PreprocessorDefinitions>`. Switching it requires a full rebuild and a
regen pass (because the generated C differs). No shared objects, no
mixing debug + non-debug translation units.

## Debug client

A thin Python client (`snesrecomp/tests/l3/rdb.py`) wraps the TCP
commands with a REPL-ish interface:

```
> connect recomp
> break BufferScrollingTiles_Layer1:label_8a55 --if iter==32
> continue
(paused at block 0x8a55, iter 32)
> dump _8 _A _27 _30
> step
> step_back 3
> write _8 0xC0   # patch state, keep going
> continue
```

Built incrementally alongside each tier.

## Oracle parity

Every tier, mirrored into the oracle debug_server for side-by-side A/B.
When a bug needs comparing, the client drives both binaries in lockstep
via the same breakpoint, inspecting diverging state directly.

## Rollout

1. Write this doc (now).
2. Build Tier 1. Exercise against the ground bug as litmus. If closed,
   stop. If not, hand off what we learned and continue.
3. Build Tier 2. Exercise against whatever Tier 1 left open.
4. Build Tier 3 only when Tier 2 runs out.
5. Tier 4 deferred indefinitely.

At each tier boundary we gain a reusable tool for every future recomp
bug. That amortization is what makes this worth building even beyond
the immediate investigation.
