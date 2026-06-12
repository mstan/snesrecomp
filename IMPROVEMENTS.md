# snesrecomp framework improvements

Backlog of toolchain / framework improvements that are NOT part of any
active in-flight task. Add new entries as they surface during work;
graduate to in-flight tasks only when explicitly scheduled.

---

## Attribution — widescreen / extra-side-space PPU model

The runner's widescreen rendering descends from **snesrev's `zelda3`**
(https://github.com/snesrev/zelda3, MIT) — `runner/src/snes/ppu.c` is a fork
of its `snes/ppu.c`, and the per-side margin machinery
(`extraLeftCur`/`extraRightCur`/`extraLeftRight`/`extraBottomCur`,
`PpuSetExtraSideSpace`) is reimplemented from that project. The dynamic
room-bounds-clamped policy each game layers on top (e.g. ALttP's
`ZeldaConfigurePpuSideSpace`) is reimplemented from zelda3's
`ConfigurePpuSideSpace`, read over the recompiled ROM's live WRAM instead of
zelda3's C globals.

Widescreen and quality-of-life *concepts* for A Link to the Past (the
widescreen fix, HUD rearrange, per-button item boxes, in-game settings menu)
were referenced from **xander-haj's `Z3R`** (https://github.com/xander-haj/Z3R)
and its commented `z3c` base — both forks of `zelda3` (MIT). We reimplement
against the original ROM under the static-recompilation model rather than
copying their C; credit and thanks to those authors.

The PPU-level capability lives in the shared runner (`runner/src/snes/ppu.c`
+ `runner/src/widescreen.{c,h}`); per-game policy lives in each game repo
behind its own opt-in config (see recomp-template/ENHANCEMENTS.md).

---

## IN FLIGHT: Option-1 cpu->S model revival (2026-05-26) — de-risked & runtime-ready

Branch `feat/cpu-s-stack-model`. Reviving the "Abandoned path" below, now
de-risked. This is the complete fix for the JSR/JSL ↔ RTS/RTL ↔ PEI-trampoline
stack-model divergence that produced the MMX Dr Light freeze, Chill Penguin
softlock, AND the Launch-Octopus fish softlock (see MegamanXRecomp
`ISSUES.md` — all three are one root cause: `D56F`'s `PLP` reads a byte
clobbered by a callee touching the absent JSR return frame).

### Key de-risking finding: the RUNTIME is already done

The Option-2 "PEI-trampoline returns" work (commit `bf8a34b`) already landed
the entire runtime dispatch surface — so Option-1 is now a **codegen-only**
change:

- `runner/src/cpu_state.c:289-343`: `g_dispatch_table` (sorted by pc24),
  `_cpu_dispatch_lookup` (binary search + variant pick by runtime m/x +
  LoROM bank-mirror fallback), `cpu_dispatch_pc_from(cpu, pc24,
  entry_s_for_miss_restore, source_pc24)` (miss → return NORMAL → host C
  stack unwinds). `tools/v2_regen.py` already emits `<prefix>_dispatch_v2.c`
  and the vcxprojs already compile it. Nothing on the runtime/build side
  needs to change.

### Push design (constants from `op.source_pc24`)

Return addr = site + insn_len; pushed value = return − 1 (RTS/RTL add 1 on
pop, matching `_emit_return`'s existing pop arithmetic).

- **JSR** (3 bytes): push `(site+2)&0xFFFF` as 2 bytes (PCH then PCL).
- **JSL** (4 bytes): push PBR=`(site>>16)&0xFF`, then `(site+3)&0xFFFF` (PCH
  then PCL) — 3 bytes.
- Synthesized call with no `source_pc24` → push a correctly-SIZED sentinel
  (balanced callees pop+ignore; the rare trampoline dispatches to a
  lookup-miss → NORMAL → C unwind).

### Invoke-path audit — EVERY path must push, or a callee pops a frame nobody pushed (this is what broke the prior attempt)

1. `codegen._emit_call` (codegen.py:1421) — 4 shapes: pinned-JSL, pinned-JSR,
   regular-JSL (PB save/restore + m/x switch), regular-JSR (m/x switch). Push
   at the start of each; pop one frame in the SKIP-N propagation block.
2. `codegen._emit_dispatch` (codegen.py:1331) — the JSL-jump-table dispatcher
   is ITSELF a trampoline: it pops its own JSL return and the handler returns
   to the dispatcher's caller's-caller. Under Option-1 it must POP its own
   pushed frame, then invoke the handler (which pops the next frame). Care
   needed: `call_with_pb_save` does not push today.
3. The two JMP/JML indirect-dispatch emitters (codegen.py ~1172 / ~1278) are
   TAIL-calls — they must NOT push (callee inherits the caller's frame).
4. `emit_function.py` tail-calls past `end:` (sibling inheritance + cross-bank,
   incl. the NLR-aware `_tail_call_stmt`) — tail-calls, NO push; `_entry_s`
   must reflect the OUTER caller's pre-push S.

### Return path: generalize the existing trampoline branch to ALL returns

`codegen._emit_return` (codegen.py:1585) already has the 3-path shape for
`_TRAMPOLINE_RETURNS`-flagged sites (NLR `_ps` / `cpu->S != _entry_s` dispatch
via `cpu_dispatch_pc_from` / balanced). For Option-1: drop the `is_trampoline`
gate so EVERY RTS/RTL pops its frame — balanced (`cpu->S == _entry_s`) pops
`frame_size` and returns NORMAL (C stack unwinds); trampoline pops+dispatches.
The `_classify_trampoline_returns` intra-procedural detector becomes
unnecessary (cpu->S frames make trampolines work naturally).

### RESOLVED ABI (2026-05-26, after ChatGPT cross-check): host_return_valid

Decision settled: **option (B)** — retire `_pending_skip` for PLA*N NLRs and
let returns flow through `cpu->S` — PLUS an explicit **`host_return_valid`**
flag so a function's RTS/RTL does NOT rely solely on `cpu->S == _entry_s` to
decide whether a real paired host-C caller exists (that comparison
false-positives when a function is *dispatched* into but happens to land at its
entry S).

`host_return_valid` (new `uint8 cpu->host_return_valid`; each function captures
`uint8 _hrv = cpu->host_return_valid;` in its prologue):

- **Direct generated JSR/JSL call** (`_emit_call`): push the hardware return
  frame AND set `cpu->host_return_valid = 1` before the C call. Callee enters
  with `_hrv = 1`.
- **Tail JMP/JML** (indirect-dispatch tail emitters, `emit_function.py`
  tail-calls past `end:`): do NOT push; set `cpu->host_return_valid = _hrv`
  (propagate THIS function's entry validity — a tail-call hands off our return
  obligation), then C-call the tail target.
- **`cpu_dispatch_pc_from`, PEI/RTL trampoline dispatch, dirty-RAM dynarec
  entry, dispatch-trampoline targets**: enter with `cpu->host_return_valid = 0`
  (no proven paired host caller) unless a paired host caller is proven.
- **RTS/RTL**: ALWAYS pop the hardware frame. Return `RECOMP_RETURN_NORMAL`
  **only when `_hrv == 1` AND the stack was balanced at entry**
  (`cpu->S == _entry_s` before the pop). Otherwise `return
  cpu_dispatch_pc_from(cpu, popped_pc24, ...)`.
- **Retire `_pending_skip`** behavior: emit PLA/PLB/PLP/PLD/etc. as normal
  `cpu->S` ops; the exposed return frame is consumed by RTS/RTL. Keep
  `_classify_trampoline_returns` for DIAGNOSTICS only (boundary/stack-drift
  tooling), not behavior.
- **Interrupts stay a SEPARATE ABI**: IRQ/NMI push the interrupt frame on
  entry; `RTI` pops the interrupt frame and is NOT treated like RTS/RTL. Do not
  route RTI through the host_return_valid return logic.
- **Dirty-RAM / executable-RAM** (if/when MMX hits it): dynarec fallback is
  allowed but MUST emit compiled code using THIS same ABI, warn, and cache by
  (PC + mode + code-hash). NO interpreter fallback. (Out of scope for the
  immediate fish-softlock fix unless a RAM-exec path surfaces.)

Validate empirically: regen MMX, repro the fish softlock, read the boundary
ring to confirm `cpu->S` stays balanced frame-to-frame (D56F enters at a stable
S, exits m=1) AND controller input + boot-to-Highway do not regress.

### Validation order (per the cross-game rule)

1. MMX: fish softlock fixed AND controller input still works AND attract/boot
   still reaches Highway (the prior attempt's regression). Measure via boundary
   ring: D56F exits m=1; no per-frame cpu->S drift.
2. Only then: SMW + LttP full regen + build + smoke test (final cross-game
   guard).

---

## Option-1 attempt #2 — boot-wedge diagnosis (2026-05-26)

Implemented the full host_return_valid ABI (commits on `feat/cpu-s-stack-model`).
It **compiles clean and runs**, but **regresses boot**: the game wedges by
~frame 456 (game mode 58), watchdog tripping every frame, call_stack pinned in
the NMI DMA-queue walker `bank_00_82C8 ← 83F1 ← 83D9 ← NmiHandler ← I_NMI`
(black screen + brief garble). Same `82C8` spin as the Dr Light freeze.

### Measured root cause (boundary-ring S-delta at the wedge)
- **`cpu->S` drifts into PAGE 2 (`$02ff`)** — should be ~`$01ff`. Net over-pop
  of ~256 bytes accumulated during boot. Page-2 stack + the DMA-queue tail at
  zero-page `$00A5` → corruption → walker spin.
- **Normal JSR/RTS pairs are correctly balanced** (per-function S-delta `+2` =
  callee pops the 2-byte frame its caller pushed — exactly right). The core ABI
  is sound.
- **The over-pops are all on the interrupt/DMA-path functions:** `84C3` `+9`,
  `83D9` `−9`, `81E3` `−12`, and `I_NMI` occasionally drains `+500..+598`. These
  are the functions that on hardware manipulate the **interrupt frame** the CPU
  pushes on NMI/IRQ entry.

### Leading hypothesis: the interrupt-frame boundary
The recompiled NMI handler does `PHP`…`RTI` (see `mmx_rtl.c` ~437-446 comment),
but the scheduler invokes it via a plain C call — `mmx_rtl.c:456 I_NMI(&g_cpu);`
(and `:395 I_IRQ(&g_cpu);`) — which pushes **no** interrupt frame on `cpu->S`.
Under Option-1 `cpu->S` is now load-bearing, so the handler's interrupt-frame
pops (`RTI` / explicit `PLA/PLP`) have nothing matching to pop → over-pop →
page-2 drift.

### Fix site (next session)
1. `mmx_rtl.c:456` (NMI) + `:395` (IRQ): push the native interrupt frame
   (PB, PCH, PCL, P — 4 bytes) on `g_cpu.S` before the handler call.
2. `codegen._emit_return` RTI path: **pop** that 4-byte frame (restore P from
   the pulled byte; discard PC/PB — host C return carries control) instead of
   the current no-pop `return _ps`.

### CAVEAT — not necessarily a one-shot fix
The deltas are messy (`−12`, `+9`, `+500`), not a clean single 4-byte frame, so
the NMI/IRQ/DMA path likely has additional stack idioms (or the `I_NMI` +500
drains are a separate TXS-reset / runaway-pop). Expect to iterate: after the
interrupt-frame push, re-measure the boundary-ring S-delta and chase the next
non-`+2`/`+3` function. The empirical loop (regen ~7min + build ~5min + boot +
boundary S-delta) is the tool.

### Update: interrupt-frame fix landed — necessary but NOT sufficient (2026-05-26)
Implemented the fix (commit `f9c8350` + `mmx_rtl.c` entry pushes): RTI pops the
native/emu interrupt frame; `mmx_rtl.c:395/:456` push it before I_IRQ/I_NMI.
**Result:** `cpu->S` no longer drifts to page 2 — it stays in page 1 (`$01ff`),
and `I_NMI`/`NmiHandler` now balance (+4 = they pop the pushed frame). BUT boot
**still wedges** in `82C8`. Re-measured per-function S-deltas show MORE
unbalanced idioms in the NMI/IRQ/DMA path:
- `bank_00_83D9` **−9** (95×, consistent — pushes 9, never pops)
- `I_IRQ` / `IrqHandler` **−14**
- `bank_00_84C3` **+13**
These are additional per-function stack idioms (PEI/PHA trampolines or
interrupt-frame manipulation) that must each be reconciled. Option-1 is
genuinely MULTI-LAYER: drift (fixed) → interrupt-frame (fixed) → per-function
NMI/IRQ/DMA stack idioms (remaining). Next: disassemble `83D9` (clearest, −9),
understand the idiom, reconcile, regen/build/re-measure, repeat for IRQ chain.

### Layer 3 characterized: the NMI DMA-processing chain (2026-05-26)
`bank_00_83D9` (the −9 leaker) is an NMI-handler **indirect dispatcher**:
`JMP ($83EB,X)` → `bank_00_83F1` (X=0) or `bank_00_8428` (X≠0), reached from
`NmiHandler`. The −9 lives in that dispatched chain
(`83F1`/`8428` → `81E3`/`82C8`) — the functions that build and walk the DMA
queue. They use a stack protocol (PEI/PHA producer-consumer across the chain,
+ the `82C8` queue-tail walk) that Option-1's plain JSR-push/RTS-pop model does
NOT capture. This is NOT a single-function fix — it's a cohesive sub-system
whose cross-function stack contract must be modeled.

Remaining layers after this: the IRQ chain (`I_IRQ`/`IrqHandler` −14,
`84C3` +13) — almost certainly the same class (interrupt-driven DMA path).

**Conclusion:** Option-1 is N-layer with N > one session. Layers fixed: drift,
interrupt-frame. Layer remaining: the NMI/IRQ DMA-processing chains'
cross-function stack protocol. Recommended approach for a dedicated session:
trace the DMA-queue stack protocol across `83D9→83F1/8428→81E3→82C8` as a unit
(what each pushes for the next to consume), then model it — likely needs the
chain treated as a known producer/consumer group, or those specific PEI
trampolines detected and emitted with `cpu_dispatch_pc`, rather than ad-hoc
per-function deltas.

### Status
Parked on `feat/cpu-s-stack-model`. Working game = Production v0.1.1 (untouched;
its exe predates the Option-1 regen). To restore the Oracle dev build to
working: `git checkout main` (commit/stash first) + regen + rebuild.

---

## Abandoned path: full cpu->S model for JSR/JSL/RTS/RTL (2026-05-24)

**Status:** Rolled back. The narrower PEI-trampoline detector (ISSUES.md
"Option 2" — static decoder-side pattern flag) was chosen as the next
attempt for the Dr Light freeze.

**What was attempted:** Model JSR/JSL pushes faithfully on cpu->S
(JSR pre-push 2 bytes = pc+2; JSL pre-push 3 bytes = PB + pc+3) and
emit RTS/RTL with a three-path exit:

1. **NLR** (`_pending_skip != NORMAL`): pop 1 frame, return `_ps`.
2. **Trampoline** (`cpu->S != _entry_s`): pop the topmost frame from
   cpu->S, compute (PB:PC+1), tail-call a new runtime helper
   `cpu_dispatch_pc(cpu, pc24)`. The helper binary-searches a
   per-game dispatch table (`<prefix>_dispatch_v2.c`) emitted by
   `v2_regen.py` after the autopromote loop. Lookup miss → return
   NORMAL → host C stack unwinds.
3. **Balanced**: pop the JSR/JSL pre-pushed frame, return NORMAL.

Symmetric pop in `_emit_call`'s SKIP-N propagation block (each
propagating level pops one frame).

**What worked:** The original Dr Light BCS-self-spin freeze (NMI
walker spinning at `$00:BA48` because `bank_04_9A02`'s PEI-trampoline
RTL leaked 6 bytes onto cpu->S, eventually corrupting the DMA queue
tail at `$00:00A5/A6`) was fixed. After the fix the same TCP-driven
repro (boot → loadstate 0 → `p1=right` 4s) no longer produces the
spin signature; call stack stays in normal cooperative-task yield.

**What broke:** X did not respond to controller input at all (frames
advanced, scheduler ticked, but X never moved — neither right nor
jump). Same behaviour on a fresh boot (no loadstate). Diagnosis (not
fully confirmed via boundary audit because the regression was caught
before that scope): the cpu->S push/pop model is symmetric for direct
JSR/JSL → RTS/RTL pairs, but several *other* emit paths invoke
recompiled functions WITHOUT going through `_emit_call`, so they
push 0 but the callee's `_emit_return` pops 2 or 3 bytes. Each such
mismatch leaks bytes on cpu->S; at hundreds of calls per frame the
scheduler's state corrupts quickly.

**Specific non-`_emit_call` entry paths that broke:**

1. `codegen._emit_dispatch` (ExecutePtr-style synthesized switch over
   a dispatch table). Each `case i:` invokes the handler via
   `emitter_helpers.call_with_pb_save`, which does NOT pre-push.
   Handler's balanced RTL pops 3 bytes from cpu->S that belong to a
   different frame.
2. `emit_function.py` tail-call-past-`end:` blocks (sibling-function
   inheritance and cross-bank tail-calls). `{ RecompReturn _tc =
   sibling(cpu); RecompStackPop(); return _tc; }` — no pre-push,
   tail-callee pops anyway.
3. JSR-caller → RTL-callee or JSL-caller → RTS-callee frame-size
   mismatch through indirect dispatch. Caller pushed 2/3, callee
   pops 3/2 → off by 1 per dispatch.

**What would need to happen to revive this approach:**

- Audit every recompiled-function entry point and ensure it goes
  through a uniform "pre-push" surface (either `_emit_call` or an
  equivalent that knows the frame size).
- Track the calling site's frame size (JSR=2, JSL=3) on every
  synthesized invocation so the callee's RTS/RTL pop matches.
- Resolve the tail-call / cross-fn-inheritance shape:
  the tail-callee's `_entry_s` must reflect the same cpu->S state
  that the original outer caller pre-pushed for, not the
  intermediate state at the tail-call site.
- Decide what to do about M/X variants that exit with a different
  return type than they entered (RTL where the caller JSR'd, etc.) —
  these are likely real ROM bugs in the asm but our model has to
  cope without crashing the host C call chain.

**Reverted scope (rollback boundary):**

- `recompiler/v2/codegen.py`: `_emit_jsr_jsl_push`,
  `_emit_skip_propagation_pop`, `_JSR_FRAME_BYTES`,
  `_JSL_FRAME_BYTES`, modifications to `_emit_call`'s four paths
  (force_variant_at JSL/JSR + regular JSL/JSR), the three-path
  rewrite of `_emit_return`.
- `recompiler/v2/emit_function.py`: `_entry_s` prologue line, HLE
  wrapper frame-pop.
- `runner/src/cpu_state.h`: `DispatchEntry` typedef,
  `cpu_dispatch_pc` declaration, `g_dispatch_table` extern, the
  full rewrite of the "Non-local return signaling" comment.
- `runner/src/cpu_state.c`: `cpu_dispatch_pc` body +
  `_cpu_dispatch_lookup` helper.
- `runner/src/cpu_trace.h`: `BD_EXIT_KIND_TRAMPOLINE` enum value.
- `tools/v2_regen.py`: the per-game `<prefix>_dispatch_v2.c` emit
  block (forward decls + sorted dispatch table + sentinel for
  empty-cfg case).
- Per-game vcxproj entries adding `gen\<prefix>_dispatch_v2.c`
  ClCompile (MMX, SMW, LttP).
- Each game's `src/gen/<prefix>_dispatch_v2.c` (delete; regen
  with reverted code won't emit it).

**Preserved (NOT reverted — these landed earlier in this session
chain, BEFORE Option 1):**

- `force_variant_at` cfg directive framework (cfg_loader,
  codegen, lowering, v2_regen Call.source_pc24 plumbing).
- snes9x-oracle disabled-by-game guardrail (cpu_state /
  emu_oracle_cmds / snes_oracle_backend) + the per-game MMX
  opt-out (config.h/.c, main.c, mmx.ini).

---

## Regen iteration time

Today (2026-05-24): a full MMX regen takes ~25 min. The pipeline
spends almost all of that in Python (`v2_regen.py`) — variant
discovery + iterative auto-promote + emit per bank. The MSVC Oracle
build on top of regen output is the smaller slice (~5 min from
clean). For sessions that iterate on `recompiler/v2/*.py` (codegen,
decoder, lowering, emit_function), the 25-min regen is the dominant
cost per loop. Two improvement lanes:

### Priority 1 — Parallel regen across banks (HIGH PRIORITY)

The 8 MMX banks (similar surface in SMW, LttP) are mostly independent
within a single auto-promote pass: each bank's `emit_bank` call
decodes + lowers + emits its own ROM range, producing a per-bank C
file plus a set of cross-bank Call demands. Coordination between
banks is only required at the pass boundary (to merge demand sets
and decide which entries auto-promote into which cfg).

Concretely: within `v2_regen.py`'s emit loop, replace the serial
`for bank, ... in parsed:` over `emit_bank(...)` with a worker pool
(`concurrent.futures.ProcessPoolExecutor`, or `multiprocessing.Pool`
sized to `cpu_count` minus 1). Each worker emits one bank's .c file
and returns the build report (suppressions, demands, const-Z folds,
etc.). Main process aggregates after all banks finish, then runs the
auto-promote merge serially before the next pass.

Expected impact: 2-4x speedup on 8-bank MMX. SMW (10 banks) and
LttP (~20 banks) scale similarly with core count.

Caveats to nail down during implementation:
- `set_name_resolver` is a module-level global in codegen.py; each
  worker process gets its own copy via fork/spawn — fine for
  process-pool, NOT thread-pool.
- `_NAME_RESOLVER` / `_UNRESOLVED_CALL_TARGETS` / etc. are
  process-local. Workers must serialize their final state back so
  the main process can union them.
- The auto-promote pass between iterations is fundamentally serial
  (merges all banks' demands), so the speedup applies to the emit
  phase only, not to the full convergence loop. Still substantial
  since emit dominates per-pass cost.

This is the higher-leverage win because it helps every workflow —
codegen.py iteration, cfg iteration, ROM-change iteration alike.

### Priority 2 — Incremental regen via bank-level dependency cache (DISCOVERY SPIKE)

Possible but the wins concentrate on a workflow that isn't the
current pain point. Approach:

- The variant-discovery pass at the top of `v2_regen.py` walks the
  ROM cheaply (no emit, no auto-promote) and produces the full
  `variants: dict[pc24 -> set[(m,x)]]` map. Partition by source bank
  to get an explicit "bank A demands (target, m, x) tuples from
  bank B" graph.
- Cache key per bank: SHA of (cfg file + relevant ROM byte range +
  cross-bank demand set + codegen.py SHA + framework version).
- Cache hit → reuse the existing emitted .c file. Cache miss → re-emit.

When this pays off:
- **cfg edit on a single bank**: typically a 1-bank rebuild. **Big
  win**, frequent workflow when humans add `name` / `exit_mx` /
  `indirect_call_table` directives.

When this does NOT pay off:
- **codegen.py edit**: framework SHA changes → every bank's cache
  invalidates → full rebuild. **No win.** This is the workflow we
  were in during the PEI-trampoline fix.
- **ROM change**: rare for static recomp.

Status: not scheduled. Worth a spike to validate the dep-graph
extraction is precise (i.e. there are no implicit cross-bank deps
the discovery pass misses). If the spike says yes, prioritize on
the next cfg-iteration-heavy session. If it says no — drop and
revisit only with new evidence.

---

## Other ideas, low priority

- Split each bank's C file into smaller TUs (one per `func` block,
  say) so MSVC `//m` parallelizes harder. Would help Oracle build
  time even without parallel regen.
- Emit LLVM IR directly + invoke `clang` instead of MSVC. Skips the
  C parser entirely. Architecturally interesting; weeks of work.

---

## Incremental re-emit per pass — gated by a callers index (2026-05-25)

**Status:** Explored and rolled back. The naive form (skip banks
whose `cfg.entries` did not grow; force all banks dirty when
`callee_exit_mx` changes) is a no-op for MMX because the auto-
promote refresh adds new `exit_mx` routes EVERY pass, so the
"force all dirty" branch always fires. Measured 298s vs A1-only
297s on MMX1 (no skips observed in 8 passes).

**What's needed for the complete form:** a per-pass callers-index,
keyed by callee `pc24` → set of caller-bank IDs. Updated when
`emit_bank` processes a bank's decoded graphs (drain every JSR/JSL
target into the index). Between passes:

1. Diff `callee_exit_mx` against the prior pass to compute the set
   of changed callee `pc24`s (new keys + changed values + removed).
2. Use the callers-index to map changed callees → set of caller
   banks whose decode would now see different post-JSR state.
3. Union with banks whose own `cfg.entries` grew during this pass's
   auto-promote.
4. The union is the dirty set for the next pass.

**Estimated win for MMX:** ~30% reduction on passes 4–7 of the
auto-promote loop, since most banks ARE clean once the
`callee_exit_mx` churn is filtered to its actual blast radius.
Passes 0–3 already small. Translates to ~100s saved on top of A1.

**Why not now:** the callers-index adds real surface area (build
during emit, drain via worker return for parallel A2, maintain
across the auto-promote refresh). For the current target (Chill
Penguin debug iteration), A1's 5× and A2's projected additional
2–4× are the bigger wins; this can wait.

**When to revisit:** if the iteration cadence after A1+A2 still
feels slow (>3 min total cycle), or when a debug session has many
cfg-only edits (those don't change `callee_exit_mx` at all, so the
naive A3 might suddenly start saving — but at that point the
disk-cache idea in §"Per-bank cache hashed on inputs" is even
stronger because it skips the decode entirely).
