# Multi-Tier Recompilation for snesrecomp — Design & Decision

**Status:** Design accepted (Option A). Implementation not started.
**Branch:** `feat/multi-tier-interp-fallback`
**Date:** 2026-06-17
**Decision owner:** project owner. **Author:** exploratory pass (Claude).

---

## 0. TL;DR

psxrecomp has a four-layer execution model (AOT native → async-built gcc DLL
shard → in-process sljit JIT shard → MIPS interpreter floor) plus a manifest
that folds runtime-discovered code back into the ahead-of-time build. We
evaluated porting it to snesrecomp.

**Verdict:**

- **The performance tiers (gcc shard + sljit JIT) are NOT worth porting.** They
  exist in psxrecomp to run code at native speed. snesrecomp recompiles a
  3.58 MHz guest to native C with LTCG and already runs at huge headroom over
  realtime (it ships turbo modes). The SPC700 *already* runs as a plain
  interpreter in-build and keeps up. A JIT for the main CPU would solve a
  problem we do not have.
- **The interpreter-fallback tier + manifest feedback IS worth porting** — not
  for speed, but to convert snesrecomp's *resolve-or-fail-the-build* wall into
  a self-healing loop, and to give games a runtime correctness floor for
  control flow the static pass hasn't covered yet.
- **It ships LIVE in Production** (project-owner decision, 2026-06-17), not
  dev-gated. The instability of the live interp↔AOT bridge is **accepted as a
  decaying transient**: each release folds the prior release's discovered gaps
  back into AOT via the manifest loop, so trap frequency — and thus the bridge's
  exposure — trends toward zero. The steady state equals full coverage (interp
  present but dormant, runtime risk ≈ 0). See §3a (operating model).
- **We will not hand-write a 65816 interpreter.** We adopt **LakeSnes** (MIT).
  snesrecomp's *original* interpreter was already a LakeSnes adaptation (ripped
  in commit `9de9855`, "-2251 LOC"), so the integration pattern is proven and
  partially recoverable from history.

**Scope chosen (Option A):** AOT (Tier 1, unchanged) + LakeSnes interpreter
fallback (Tier 2) + manifest feedback into the cfgs/recompiler. **No gcc shard,
no sljit, no native-code persist cache.**

---

## 1. What psxrecomp actually built, and why

psxrecomp's tier system (`SLJIT.md`, `docs/overlay-*.md`,
`runtime/src/overlay_loader.c`, `overlay_sljit.c`, `dirty_ram_interp.c`,
`code_provider.c`, `autocompile.c`) exists to solve a **PSX-specific** problem:
**overlays**. PSX games DMA code off the disc into RAM at runtime and overwrite
it. A pure AOT recompiler *cannot see that code at build time*. So psxrecomp
needs a runtime path:

1. **Tier 1 — AOT native.** Main EXE + BIOS recompiled C++→C→native. Plus
   async-built **gcc DLL shards** for overlays once captured.
2. **Tier 2 — sljit JIT shard.** `overlay_sljit.c` JITs a captured fragment
   in-process on first miss (for machines with no compiler toolchain).
3. **Tier 3 — interpreter floor.** `dirty_ram_interp.c`, a MIPS R3000A
   interpreter; the correctness floor and the differential oracle that
   validates JIT/AOT output before it is trusted.
4. **Feedback.** `overlay_captures.json` (the portable coverage manifest) +
   `<exe>_full.ranges` are fed to `tools/compile_overlays.py`, which re-runs the
   recompiler in evidence-scoped mode and emits the gcc DLL — folding a user's
   JIT'd long-tail back into native code for everyone.

Dispatch is **PC-keyed**: `overlay_loader_dispatch(phys)` walks a candidate
chain (gcc DLL > sljit shard > interp), re-hashing live bytes every call for
self-modify safety, gated by a same-state differential against the interpreter.

Reusable *infrastructure* (the CPU-agnostic ideas): the provider seam
(`CodeProvider`), content-keyed candidate dispatch with per-call validation,
the precision-over-recall differential gate, the three-namespace cache, the
async-compile state machine, position-independent persisted blobs. ~2.5–3.5K
lines of genuinely portable C. Everything else (MIPS decode, GTE, the
interpreter, the sljit emitter) is ISA-specific.

**The hardest, least-finished piece of psxrecomp is the interp↔compiled call
contract** — their open `RECURSION_BUG` (branch `bug/recursion`) is exactly a
host-stack frame leak at the boundary where an interpreted call enters compiled
code (or vice versa). Any multi-tier port must get this contract right
regardless of ISA. See §6.

---

## 2. Why SNES is different — the crux

| Dimension | psxrecomp (PSX) | snesrecomp (SNES) |
|---|---|---|
| Code source | Disc + RAM overlays (streamed) | Cartridge ROM, fully visible at build time |
| Code AOT can't see | Large, first-class (overlays) | Tiny: a few WRAM-decompressed routines, hand-HLE'd today (`gen_stubs.c`) |
| Speed need for a JIT | Real on weak hardware | None — already far above realtime; turbo modes ship |
| Existing interpreter | `dirty_ram_interp.c`, live | **Deleted** (`9de9855`); SPC700 interp still in-build |
| Unresolved control flow | Falls to interp | **Hard build error** (stub-lint in `tools/v2_regen.py`) |
| Execution model | PC-driven blocks, dispatch by PC | **No live PC** — direct host-C calls; guest stack *is* the host call stack; returns via `RecompReturn` NLR enum; up to 4 width-variants per function |

Two takeaways:

1. **The thing psxrecomp's tiers fundamentally solve (runtime-materialized
   code) barely exists on SNES.** Cartridge ROM is all there at build time.
2. **Where psxrecomp's model is JIT-friendly (uniform PC→block), snesrecomp's
   model is JIT/interp-*hostile*** (no live PC, host-call-frame stack, NLR enum,
   width variants). This makes the interp tier the most valuable *and* the most
   expensive piece to graft — and the perf tiers not worth grafting at all.

### What snesrecomp's pain actually is

Not speed — **the resolve-or-fail-the-build treadmill.** Every indirect jump,
computed target, or missed function must be cleared by hand-authored cfg
directives (`indirect_dispatch`, `indirect_call_table`, `func`, variant
pruning…) or the build fails (`_STUB_MARKERS` lint, `tools/v2_regen.py:106`).
This is the dominant cost of bringing up a new game. A runtime fallback +
manifest feedback turns that wall into: *tier down → record → fold back into the
next regen.*

---

## 3. Decision — Option A (scoped)

Adopt **only** the interpreter floor + the feedback loop:

```
Tier 1   AOT native C            (existing — unchanged)
Tier 2   LakeSnes interpreter    (NEW — the correctness floor + discovery probe)
Feedback manifest JSON → cfg/recompiler  (NEW — closes the loop to re-AOT)
```

**Explicitly out of scope** (and why):

- **gcc-compiled DLL shards / async compile / native persist cache** — speed
  optimization we don't need; adds Windows-only spawn machinery and a second
  build path.
- **sljit JIT + the ~1.1K-line second emitter + parity burden** — psxrecomp
  itself notes the permanent cost of maintaining two emitters in lockstep. For
  a guest already running far above realtime, this buys nothing.
- **Content-keyed self-mod re-hash dispatch** — relevant to overlays; SNES
  WRAM-code is rare and handled as a known, bounded set (§7 first customer).

This keeps the win (robustness + bring-up velocity) and drops the cost that has
no SNES payoff. It is the *complete* solution to the problem SNES actually has,
not a stripped-down solution to a problem it doesn't.

### 3a. Operating model — live in Production, decaying instability

Tier 2 is **always-on in every config, Production included.** It is the
shipped runtime floor: a guest control-flow target the static pass hasn't
covered tiers down to the interpreter and the game keeps running (perhaps with a
glitch) instead of hard-failing.

The instability of crossing the interp↔AOT bridge at runtime (§6) is **knowingly
accepted, as a transient that the feedback loop actively retires:**

```
ship vN  ──run──▶  interp catches gap G, records it to the manifest
                          │
                   offline: ingest → cfg directive → oracle-verify
                          │
ship vN+1 ◀── regen ──────┘   gap G is now Tier-1 AOT; that trap never fires again
```

Each release closes the prior release's discovered gaps, so the set of
runtime-reachable traps shrinks monotonically and the live bridge is exercised
less and less. **As coverage → complete, trap frequency → 0, and the accepted
instability → 0** — converging on the same end state as "full coverage, interp
dormant" (the risk-0 case), but reached gradually with a *running* game at every
step instead of a hard crash at each uncovered target.

Because the shipped product depends on the bridge until coverage is complete,
two things are non-negotiable (vs the dev-tool framing where they'd be optional):

1. **Bridge failures must be *contained*, never silently corrupting** (§6 —
   graceful degradation). An uncrossable boundary degrades to a recoverable
   glitch or a clean controlled stop, never to silent state corruption that
   propagates into save data.
2. **Promoted manifest entries must be oracle-verified before they become AOT**
   (§8, §10.5) — the loop must not launder a runtime mis-execution into a
   trusted static translation.

---

## 4. The interpreter — LakeSnes (MIT)

**Choice: LakeSnes** (`github.com/angelo-wf/lakesnes`, MIT, written in C).

### Why LakeSnes over the alternatives

| Core | License | Lang | Fit |
|---|---|---|---|
| **LakeSnes** `snes/cpu.{c,h}` | **MIT** | C | Clean callback bus; **already adapted in this repo once** |
| ares `wdc65816` | ISC (permissive) | C++ | Accurate but coroutine/cycle-stepped, tightly coupled to ares scheduler; heavier to extract |
| bsnes / higan / Mesen | GPL | C++ | License-incompatible with shipping our binaries |
| snes9x | non-commercial | C++ | License-incompatible; previously removed from this repo |
| Lib65816 / emu65816 | (varies) | C/C++ | Less proven; no existing integration |

### Provenance — we've done this before

The interpreter ripped in `9de9855` ("Rip cpu.c 65816 interpreter dead code,
-2251 LOC") has function names identical to LakeSnes's CPU core:
`cpu_runOpcode`, `cpu_doOpcode`, `cpu_adrImm`, `cpu_adrDp`, `cpu_adrIdp`,
`cpu_adrIdx`, `cpu_adrIdy`, `cpu_setZN`, `cpu_doBranch`, `cpu_pullByte`,
`cpu_pushWord`, `cpu_doInterrupt`. So the original snesrecomp interpreter *was*
LakeSnes, adapted to the runner's bus. It was ripped only because, post-v2,
"`cpu_runOpcode` had zero external callers" — i.e. it was correct and working,
just unused once everything went pure-AOT. **We can recover the integration
shape from `9de9855^:runner/src/snes/cpu.c`** and re-target it at today's
`CpuState`.

### Integration shape — clean

LakeSnes's CPU is a single-instruction stepper with a **callback bus**:

```c
typedef uint8_t (*CpuReadHandler)(void* mem, uint32_t adr);
typedef void    (*CpuWriteHandler)(void* mem, uint32_t adr, uint8_t val);
```

The struct carries individual flag bools (`c z v n i d xf mf e`), 16-bit regs
(`a x y sp pc dp`), 8-bit (`k`=PB, `db`=DBR), and `cpu_runOpcode()` executes one
instruction. Integration is two mechanical pieces:

1. **Bus:** point `read`/`write` at thin shims over the existing HLE bus
   `cpu_read8(cpu,bank,addr)` / `cpu_write8(...)` (`cpu_state.c`) so the interp
   sees the same `g_ram[]` and the same MMIO→PPU/APU/DMA routing the AOT code
   sees. **One bus, one memory map, zero divergence.**
2. **State map:** a `CpuState` ⟷ LakeSnes-`Cpu` adapter (mechanical field
   copy + `P`↔bool-flags). Width semantics already centralized in
   `cpu_state.h` typed helpers; LakeSnes uses `mf/xf` bools — direct mapping.

### License hygiene

LakeSnes is MIT. The current tree has **no LakeSnes attribution** (it was ripped
along with the code). Re-introduction MUST add a `THIRD_PARTY_ATTRIBUTION.md`
(psxrecomp has one as a model) carrying the LakeSnes MIT notice, and keep the
license header in the vendored `cpu.c`/`cpu.h`.

---

## 5. Architecture for snesrecomp

```
                       ┌──────────────────────────────────────────┐
   frame entry  ──────▶│ Tier 1: AOT native C (g_dispatch_table,   │
   (I_RESET/I_NMI/      │ direct JSR/JSL host-C calls)             │
    RtlRunFrame)        └───────────────┬──────────────────────────┘
                                        │ unresolved control flow
                                        │  (dispatch_oob / bank-miss
                                        │   stub / WRAM-code entry)
                                        ▼
                       ┌──────────────────────────────────────────┐
                       │ Tier 2: LakeSnes interpreter              │
                       │  - load CpuState → Cpu, pc = trap target  │
                       │  - bus → cpu_read8/cpu_write8 (HLE)       │
                       │  - step until return-to-AOT (see §6)      │
                       │  - on exit: sync Cpu → CpuState           │
                       │  - RECORD the discovery to the manifest   │
                       └───────────────┬──────────────────────────┘
                                        │  (offline, between runs)
                                        ▼
                       ┌──────────────────────────────────────────┐
                       │ Feedback: manifest JSON → cfg directives  │
                       │  ingest tool promotes discovered entries  │
                       │  / authorizes indirect_dispatch → re-AOT  │
                       │  so next build covers it as Tier 1        │
                       └──────────────────────────────────────────┘
```

The interpreter is both the **runtime safety net** (the game keeps running
instead of no-op'ing a `dispatch_oob` or failing the build) and the **discovery
probe** (it records exactly which guest PCs were reached so the static pass can
absorb them).

---

## 6. The interp↔AOT bridge — the hard part

This is the single highest-risk component (it is the part psxrecomp has *not*
fully solved). snesrecomp's model makes it harder than psxrecomp's, but
snesrecomp has already built most of the primitives we need.

### The mismatch

- **AOT does not keep return PCs on the SNES stack** — JSR/JSL/RTS/RTL are host
  C call frames; `cpu->S` only carries data the program itself pushes
  (`cpu_state.h:118-145`).
- **A real interpreter DOES push return addresses on `cpu->S`** (hardware
  behavior).

So every crossing must reconcile the guest stack. snesrecomp already ships the
exact helpers for this:

- `cpu_push_jsr_return_frame(cpu)` / `cpu_push_jsl_return_frame(cpu)` — model the
  2/3-byte hardware return-frame push (`cpu_state.h:347-371`).
- `cpu_push_interrupt_frame(cpu)` — the 3/4-byte IRQ/NMI frame
  (`cpu_state.h:334-345`).
- `cpu_dispatch_pc(cpu, pc24, entry_s_for_miss_restore)` — binary-search
  `g_dispatch_table` for a PC24 and invoke the correct `(m,x)` variant
  (`cpu_state.h:426`). **This is the bridge primitive: "enter AOT at a guest
  PC."**
- `host_return_valid` + `_entry_s` watermark — the existing discipline for
  deciding whether an RTS/RTL host-returns or dispatches on a popped PC
  (`cpu_state.h:56-66`).

### The contract (proposed)

Mirror psxrecomp's "run one block, re-enter dispatch," adapted to our call model:

1. **Enter Tier 2** at a trap with a known `pc24` and the live `CpuState`. Map
   to a LakeSnes `Cpu`; set `pc=pc24 low16`, `k=bank`; record `S_enter = S`.
2. **Step** with `cpu_runOpcode()`:
   - On **JSR/JSL** to a `pc24` that **is** in `g_dispatch_table`: don't
     interpret into it. Sync state back, `cpu_push_jsr/jsl_return_frame`, call
     `cpu_dispatch_pc(pc24)`, then resume interpreting after it returns. (Keeps
     compiled code running compiled — correctness parity + speed.)
   - On JSR/JSL to an **unknown** `pc24`: interpret into it (record it).
   - On **RTS/RTL** when `S` has unwound back to `S_enter`: the interpreted
     routine has returned to its entry depth → **exit Tier 2**, sync `Cpu →
     CpuState`, return `RECOMP_RETURN_NORMAL` to the AOT trap site.
3. **Interrupts** that fire while in Tier 2 use `cpu_push_interrupt_frame` then
   `cpu_dispatch_pc` to the vector (or interpret if the vector body is unknown).

### Width variants

`cpu_dispatch_pc` already selects `variant[(m<<1)|x]` from the live flags, so
entering AOT from the interpreter picks the right width-variant for free. The
interpreter tracks `mf/xf` natively, so the flags are always current at a
crossing.

### Why this is tractable here

We are not retrofitting a free-running interpreter that interleaves with AOT at
arbitrary instruction boundaries (psxrecomp's general, and still-buggy, case).
We enter Tier 2 **only at well-defined trap sites** with a clean `_entry_s`
watermark, and we hand back at a **balanced stack**. The crossing rules reuse
machinery the trampoline-return path already exercises in production. The risk
is real but bounded; see §10.

### Avoiding psxrecomp's `RECURSION_BUG` (mandatory, since we ship live)

psxrecomp's open boundary bug is an *unbounded host-stack leak*: its interpreter
**nests** a host call (`psx_dispatch_game_compiled`) on JAL/JALR instead of
unwinding, so every interp→compiled crossing leaks a host frame chain; over a
long session the host stack overflows. Because we ship the bridge live, we must
not reproduce this. The contract is designed against it:

- **Re-entry is bounded by guest call depth, not by crossing count.** When the
  interpreter hits a JSR/JSL into a known AOT entry it bounces through
  `cpu_dispatch_pc` (one host frame that *returns*), it does not stack a new
  permanent interpreter context. interp→AOT→interp→AOT nesting is allowed but
  its depth equals the guest's own call depth at that instant — which the guest
  program bounds (the SNES stack is 256 bytes / bank 0). Bounded re-entry is
  fine; only *leaking* (frames that never unwind) is the bug.
- **Every Tier-2 exit asserts a balanced stack** (`cpu->S == S_enter` at the
  return-to-AOT point). A drift is the leak signature; it is caught at the
  crossing, not 14 minutes later as an overflow.
- **No host recursion that isn't matched by a guest return.** The only way back
  into AOT from the interp is a frame-helper-paired `cpu_dispatch_pc` call that
  returns, or a balanced RTS/RTL exit. There is no "dispatch and keep going on
  the same host frame" path.

### Graceful degradation (the live-in-Production safety requirement)

A boundary the bridge cannot cross correctly must fail *contained*, never
silently corrupt. Order of preference at any Tier-2 difficulty:

1. interpret correctly (the normal path);
2. recoverable glitch (e.g. an unhandled instruction → log + best-effort) — the
   game limps, the gap is recorded;
3. controlled stop (assert/soft-reset) **before** writing corrupted state back
   to `CpuState`/`g_ram`/SRAM.

What is forbidden: a half-crossed boundary that writes a drifted `cpu->S`,
wrong-variant register state, or partial RAM back into the live system and lets
it propagate. Save-data integrity is the floor — see §10.4.

---

## 7. Trap sites — where Tier 2 is entered

| Site | Today | With Tier 2 |
|---|---|---|
| `dispatch_oob` (indirect index past authorized `count`, or unresolved `IndirectGoto`) — `cpu_trace_dispatch_oob`, `cpu_trace.c:1682` | no-op in release, continues wrong | enter interp at the computed target; record |
| Call to a bank not in the cfg set | hard build-error stub (`unresolved_stubs_v2.c`, lint at `v2_regen.py:106`) | optionally emit a *tier-down* stub instead of failing the build (config-gated; default keep-strict for shipped games) |
| WRAM-decompressed code (e.g. SMW `$7F:8000`) | permanent hand-HLE (`gen_stubs.c`) | NOT a viable interp customer — see note |

> **Correction (2026-06-17, after Phase 1a).** The WRAM stubs were assumed to
> be the smallest "can't-AOT" customer, but inspection shows
> `SmwRunDecompressFromWRAM` / `_Entry2` are *behavioral* HLE
> (`ResetSpritesFunc(0)` / `(100)` — clear OAM Y), **not** runners of live `$7F`
> bytes. The real decompressed bytes aren't guaranteed present, and "clear
> sprites" wouldn't exercise the JSR-into-AOT bounce anyway. So the bridge
> mechanics were instead proven by a **deterministic contract harness**
> (`tests/interp816/bridge_test.c`, fake bus + fake compiled entry), and the
> first *production* trap site is `dispatch_oob`.

---

## 8. Manifest — schema & feedback

Model on psxrecomp's `overlay_captures.json` but slimmer (no captured bytes — our
bytes are in the ROM/known WRAM image). Written by the runner at trap sites
(extend the existing `cpu_trace` capture), read by an offline ingest tool.

```json
{
  "schema": "snesrecomp tier2 coverage v1",
  "rom_title": "SUPER MARIOWORLD",
  "discoveries": [
    {
      "site_pc24": "0x00C0DE",        // where AOT tiered down
      "site_kind": "dispatch_oob",     // dispatch_oob | bank_miss | wram_code
      "target_pc24": "0x0398F1",       // the entry the interp ran
      "entry_mx": "M1X1",              // (m,x) at entry → variant to emit
      "reached_pcs": ["0x0398F1", "..."], // strongest entry evidence
      "hit_count": 1421,
      "first_frame": 2687
    }
  ]
}
```

**Feedback path (offline):** an ingest tool (model on the existing
`tools/v2_regen.py` `_autopromote_targets` fixpoint and the
`cfg_override_*`/`smwdisx_crosscheck` proposers) reads the manifest and emits
cfg-ready directives:

- a new `func bank_BB_AAAA <addr>` for each discovered entry, or
- an `indirect_dispatch <site> <count> idx:<X|Y> tables:<...>` authorization
  when the discoveries cluster behind one indirect site.

Default to **human-in-the-loop paste** (matching the existing
`cfg_override_*` proposers), not auto-apply — keeps a human audit between
"observed at runtime" and "trusted as code," which is the project's existing
discipline.

This is "the manifest fed back into the base compiler," done the snesrecomp way:
discoveries become cfg seeds, the next regen makes them Tier 1.

---

## 9. Build & config

- **Vendor** LakeSnes `cpu.{c,h}` under `runner/src/snes/` (or `lib/lakesnes/`),
  add to `runner/runner.cmake` and `src/*.vcxproj` source lists. C, no new deps.
- **Gating:** Tier 2 is a correctness floor — it should be **always-on in all
  configs including Production|x64**, unlike the `SNESRECOMP_TRACE` debug rings.
  The interpreter core is small and never runs unless a trap fires, so it has no
  steady-state cost.
- **Manifest recording must be its own lightweight always-on path, NOT gated
  behind `SNESRECOMP_TRACE`.** Trace (the 2 GB debug rings) is excluded from
  Production (`smw.vcxproj`); but since Production runs are now the primary
  coverage-harvesting source (§3a), the trap→manifest recorder has to live in
  the shipped build. Keep it cheap: a bounded in-memory set of
  (site, target, mx, hit_count) flushed to a small JSON next to the save dir on
  exit. The heavy trace rings stay dev-only; only this slim recorder ships.
- Keep the strict `_STUB_MARKERS` build-error default for shipped titles; the
  "tier-down instead of fail" relaxation is an opt-in cfg/regen flag used during
  bring-up.

---

## 10. Risks & open questions

1. **The bridge (highest risk, and now SHIPPED LIVE).** The interp↔AOT crossing
   is the part psxrecomp has not fully solved (`RECURSION_BUG`). Per the
   project-owner decision it ships in Production, so this risk is **accepted as a
   decaying transient** (§3a), not avoided. Mitigations: the anti-leak contract
   (§6 — bounded re-entry, balanced-stack exit assertions, no unmatched host
   recursion); graceful contained failure (§6); enter only at bounded trap sites
   with `_entry_s` watermarks; prove on the single WRAM-code function first (§7);
   and the feedback loop actively retiring each discovered gap so the live bridge
   is exercised less over time.
2. **Stack reconciliation correctness.** AOT keeps no return PCs on `cpu->S`;
   the interp does. Every crossing must use the frame helpers consistently or
   `cpu->S` drifts (the historic "DB=$C0 at ProcessGameMode" class). Needs a
   stack-balance assertion at every Tier-2 exit.
3. **Decimal & emulation mode parity.** LakeSnes implements full 65816
   (decimal mode, E-flag). The AOT path's coverage of these must match at the
   crossing or behavior diverges. The differential oracle (`snes-oracle`) is the
   referee. Low risk since LakeSnes is the accurate side.
4. **Save-state.** Tier-2 interpreter state must serialize/restore. Since Tier 2
   only runs transiently inside a trap and exits at a balanced stack, a save
   should never land *mid-interp*; assert this rather than serialize interp
   internals.
5. **Determinism vs oracle.** The interp must produce bit-identical results to
   what an AOT body would, or the manifest could promote a target whose AOT
   translation then diverges. Validate promoted entries against the oracle
   before trusting (mirrors psxrecomp's same-state differential gate).
6. **Performance is explicitly a non-issue** — but confirm a worst-case
   pathological game that tiers down on a hot path still hits 60fps. The SPC700
   interpreter keeping up is strong prior evidence it will.
7. **Two memory maps risk = none, by design.** The interp bus routes through the
   *same* `cpu_read8/cpu_write8` as AOT. Do not let it grow a private map.

---

## 11. Phased plan

- **Phase 0 — vendor + attribution. ✅ DONE (5d46d43).** LakeSnes core vendored
  as `runner/src/snes/interp816.{c,h}` (recovered from `9de9855^`, namespaced,
  callback bus, debug stripped), `THIRD_PARTY_ATTRIBUTION.md` added, validated
  by a directed-opcode harness (`tests/interp816/interp816_test.c`, 17/17).
  Game-build wiring deferred to Phase 1b (the `interp816_opcode_hook` seam is
  undefined until the bridge provides it). *No bridge yet.*
- **Phase 1a — the bridge spike (deterministic harness). ✅ DONE (5b781ec).**
  `interp_bridge.{c,h}` + `tests/interp816/bridge_test.c`: enter → step →
  JSR-into-AOT bounce (`cpu_dispatch_pc`) → RTS-past-`s_enter` → exit, with a
  balanced-stack check and full `CpuState`↔`Interp816` sync. 12/12, no game.
  This was the go/no-go gate. (Done with a fake bus/dispatch instead of the
  WRAM customer — see §7 correction.)
- **Phase 1b — wire a real trap site + playtest.** Route `dispatch_oob`
  (`cpu_trace.c`) into `interp_bridge_run` (push a sentinel return frame, then
  run), compile `interp816.c` + `interp_bridge.c` into one game build, and
  owner-playtest. First time the bridge carries a real game.
- **Phase 2 — manifest recording. ✅ DONE.** Always-on tier-down coverage
  table in `interp_bridge.c` keyed by (site, target, m/x), tracking
  clean-return vs contained-bail counts + frame span, with a resolved-landing
  capture (an indirect-goto records the *dynamically resolved* target, not the
  JMP site). `Tier2CoverageDumpJson` embeds it in the unified post-mortem
  (`last_run_report.json`); `Tier2CoverageWriteManifest` writes the slim
  standalone `build/tier2_coverage.json` (schema "snesrecomp tier2 coverage
  v1"). Wired into SM's always-on `recomp_post_mortem_dump` (so it harvests on
  normal exit, atexit, SEH crash, and on-demand TCP — not gated behind
  `SNESRECOMP_TRACE`). Recording adds no signature change → no regen, runtime
  rebuild only. Interp harness still 17/17 + 20/20.
- **Phase 3 — profile-guided AOT + offline audit. ✅ DONE.** The
  manifest-driven emitter accepts repeatable `--profile-manifest` inputs.
  Clean-only target/MX observations become optional AOT roots and participate
  in deterministic cache hashing; bailed observations are excluded. A profile
  can only select an optimization: missing or rejected variants still execute
  through authoritative LLE. `tools/tier2_ingest.py` audits the same manifest,
  human-in-the-loop (prints, never edits), in three buckets:
  - **OPTIONAL BOUNDARY** — clean-only discoveries whose target has no `func`
    yet → paste-ready `func bank_BB_AAAA <addr16>` grouped by `bankBB.cfg`.
    These improve naming/slicing but are deliberately not reachability roots.
  - **SITE NEEDS AUTHORIZATION** — clean discoveries whose target is *already*
    a `func` → the SITE needs an `indirect_dispatch`/`indirect_call_table`
    directive (flagged, not fabricated — the index reg / table layout isn't in
    a runtime tier-down).
  - **INVESTIGATE** — bailed discoveries → bug leads, *never* promoted (no
    laundering a mis-execution into trusted AOT).
  Validated on the real SM manifest + a synthetic 3-branch fixture.
  Close the loop: discover → profile → regen → validate exact behavior.
- **Phase 4 — bank-miss tier-down (opt-in).** The build-error relaxation for
  bring-up, behind a regen flag; shipped titles stay strict.

- **Phase 4 — bank-miss tier-down. ✅ DONE (opt-in).** `v2_regen.py
  --tier-down-stubs` emits each cross-ROM-bank unresolved-function stub
  (`unresolved_stubs_v2.c`) as `interp_tier_dispatch_bank_miss` — running the
  real ROM bytes through the interpreter instead of the no-op
  `cpu_trace_unresolved_stub_trap`. A bail falls back to the same
  `cpu_unresolved_abandon_balanced` drop (never worse); every fire records to
  the manifest as a distinct `bank_miss` kind. Default OFF → shipped titles
  stay strict; no effect on any game that regens without the flag. SM opted in
  (14 stubs converted, built clean). **Finding:** none of SM's 14 bank-miss
  stubs are reached on the boot→attract path (0 `bank_miss` fires), so Phase 4
  is a no-op *for SM's current wall* — a useful negative result that rules out
  untranslated bank-miss functions as the blocker. SM's wall remains the
  `$0FE8B7` indirect-goto (`$0012=$FFFF` upstream corruption), unchanged.

Phase 1a was the decision point and it **passed** — the bridge contract holds on
the isolated case. (Had it proven intractable, the fallback was discovery-only:
record at the trap with no resuming interp; the manifest still helps but the
game can't continue past a genuinely-dynamic target until regen.)

---

## 12. Effort estimate

| Phase | Effort | Risk |
|---|---|---|
| 0 vendor + harness | small (days) — code recoverable from history | low |
| 1 bridge spike | medium (the hard part) | **high** |
| 2 dispatch_oob | medium | medium |
| 3 manifest + ingest | medium | low |
| 4 bank-miss opt-in | small | medium |

The perf tiers we declined (gcc shard + sljit + persist) would have added
~1.5–2K lines of new emitter + Windows-only async/persist machinery and a
permanent two-emitter parity burden, for zero SNES benefit.

---

## 12a. Super Metroid integration + findings (2026-06-18)

SMW is fully covered, so its tier is dormant. **Super Metroid** (mid-bring-up,
many coverage gaps) was the real test. Branch `integ/sm-interp`, off SM's engine
`dev/super-metroid` (19cb7cb) — *not* mergeable with main: SM diverged and
evolved the `cpu->S` frame ABI (`host_return_valid` = pushed frame size 0/2/3)
and already built **`cpu_unresolved_abandon_balanced`** — a record-and-safe-drop
handler for unresolved transfers (with a `CpuUnresolvedAbandonDumpJson`
post-mortem). That is exactly discovery-only (Option B) already in place.

**What we built on it:** `interp_tier_dispatch_balanced` *upgrades* the abandon
— interpret the target; on a bail fall back to abandon (never worse). Hooked
two codegen surfaces: the absolute-indirect terminal default (`_target`) and the
**unresolved IndirectGoto** (re-interpret from the `JMP` site itself, so
interp816 decodes the indirect jump and reads the pointer at runtime). SM regen
converted **225** sites; built clean under mingw gcc (third compiler — WSL-gcc,
MSVC, mingw all clean).

**Result — the tier fires, fails safe, and is a diagnostic:** SM hit the real
gap `$0FE8B7` at frame 2863; the tier counter recorded it. The PC-trail
(`SNESRECOMP_INTERP_TRACE=1`) root-caused the bail: `$0FE8B7` = `JMP ($0012)`,
and `$0012` held **`$FFFF`** (garbage) → the interpreter faithfully jumped to
`$0F:FFFF`, ran into bank-`$0F` garbage, looped, hit the step cap, and fell back
to abandon (SM spun at f2911, *same as baseline* — no regression).

**Key correction:** an earlier hypothesis ("synchronous interp can't satisfy
hardware-wait routines") was **wrong** — refuted by the trail. This is neither a
hardware-wait nor an interp mis-decode. The interp executed correctly; the
blocker is an **upstream recomp-state divergence** (`$0012` should hold a valid
target pointer but is uninitialized by f2863). The tier even served as a
*diagnostic*, exposing the exact `JMP ($0012)=$FFFF` idiom the AOT abandon path
had silently dropped.

### Methodology pivot — holistic tooling, not hand-hunting

Chasing "who should set `$0012`" by hand is a blind gap. Per
`recomp-template/.../PRINCIPLES.md` (rule 2 state-over-theory, rule 3 first
divergence, rule 10 build-tools-not-guesswork): the systematic move is a
**differential first-divergence finder** — run the recomp and the `snes-oracle`
(per-frame WRAM trace) over the same input and report the *first frame + address*
where WRAM diverges. That pinpoints where `$0012` (and any gap) first goes wrong
mechanically, for every site, instead of one hand-traced symptom. The interp
tier *feeds* this: it surfaces the gaps (which PCs tier down, with the live
state) as worklist input. Next step is to build/extend that differential
harness, not to hand-hunt `$0012`.

### Differential harness — built, and the timing verdict

Built the free-run (not lockstep) version: `snesref` widened to a general
low-WRAM per-frame trace + headless capture; an env-gated recomp-side per-frame
WRAM trace in `common_rtl.c` (same jsonl shape); `tools/wram_diff.py`
(first-divergence, with power-on-fill masking — snes9x fills WRAM `$55`, recomp
zero-inits) and `tools/align_scan.py` (offset detector).

**Verdict (data, on SM): the recomp and snesref low-WRAM do NOT align** — no
frame offset gives clean agreement (best ~46% of recomp-written bytes match at
*any* oracle frame; "best" offsets inconsistent at -18/-87/-7). An HLE/frame-
driven recomp vs a cycle-accurate oracle diverge in low-WRAM *pacing and values*
too much for a per-frame whole-WRAM diff to isolate a single first divergence —
exactly the timing fragility that got the old in-process lockstep oracle
removed. The harness works mechanically; the *signal* is too timing-confounded
for SM's boot.

This is the "adjust or abandon" outcome (anticipated). Adjustments worth trying:
(a) **targeted semantic-state compare** — diff a small set of known-stable game
vars (from the snesrev/sm decomp symbols), not all of `$0000-$1FFF`, since
scratch/RNG/timers/buffers legitimately differ between an HLE recomp and a
cycle-accurate core; (b) lean on the **interp tier's gap worklist + post-mortem**
as the primary signal rather than whole-WRAM diff. Instruction-level lockstep
(the removed approach) is *not* recommended — it's the fragility this confirmed.

### Gap-worklist loop — built + validated (2026-06-18)

Decision (owner): drive bug-finding from the **interp tier's own gap worklist**,
not a whole-WRAM differential (the timing verdict above killed that for SM).
Phases 2 + 3 built on that basis (pure runtime + offline tool; no regen).

Validated against a live SM run: the tier fired once (the only gap SM's
boot→attract execution actually reaches), and the manifest captured it exactly:

```
{"site_pc24":"0x0FE8B7","target_pc24":"0x0FFFFF","entry_mx":"M0X0",
 "site_kind":"indirect_goto","clean_hits":0,"bail_hits":1,"first_frame":2863}
```

`target_pc24 = 0x0FFFFF` is the **resolved-landing capture** working — it
records where the garbage `JMP ($0012)=$FFFF` actually jumped, mechanically
confirming the upstream-corruption finding (no hand-tracing). The run then
reproduced SM's known wall verbatim (spin at f2911 → STATUS_BAD_STACK at
`bank_00_82C5`), and the manifest was harvested via the SEH post-mortem path —
so it survives a crash. `tier2_ingest.py` correctly routes `$0FE8B7` to
**INVESTIGATE** (a bug lead, not a promote candidate), which is exactly right:
SM's current wall is upstream state corruption, not a coverage gap. The PROMOTE
path (auto-clear clean coverage gaps → `func` directives) is validated by a
synthetic fixture and will activate for SM as bringup reaches more of the 225
tier sites with valid targets.

### Tuning / status
- Step cap is tunable (`SNESRECOMP_INTERP_STEP_CAP`, default 2M). A proper fix
  detects the tight repeating-PC loop and bails early (future work).
- The PC-trail trace is opt-in (off in normal builds).
- Committed on `integ/sm-interp` (`c90ab99`). SM game-repo `src/gen` is
  gitignored (regenerated). SMW Phase 1b is on `feat/multi-tier-interp-fallback`.

---

## 13. Source map (where to look)

- **psxrecomp reference:** `F:\Projects\psxrecomp\SLJIT.md`,
  `psxrecomp\docs\overlay-recompilation-design.md`, `overlay-plan.md`;
  `runtime\src\{overlay_loader,overlay_sljit,dirty_ram_interp,code_provider,
  autocompile,overlay_capture}.c`.
- **snesrecomp bridge anchors:** `runner/src/cpu_state.h` (`CpuState`,
  `RecompReturn`, `cpu_dispatch_pc`, `cpu_push_*_frame`, `host_return_valid`),
  `runner/src/cpu_state.c` (`cpu_read8/16`, `cpu_write8/16`, `cpu_dispatch_pc`).
- **Trap sites:** `runner/src/cpu_trace.c` (`cpu_trace_dispatch_oob`),
  `src/gen/unresolved_stubs_v2.c`, game `src/gen_stubs.c` (WRAM-code HLE).
- **Recompiler feedback:** `tools/v2_regen.py` (`_autopromote_targets`,
  `_STUB_MARKERS` lint), `recompiler/v2/cfg_loader.py` (directive grammar),
  `tools/cfg_override_*`, `tools/cfg_override_smwdisx_crosscheck.py`.
- **Interpreter origin:** `git show 9de9855^:runner/src/snes/cpu.c` (the
  ripped LakeSnes-derived core); upstream `github.com/angelo-wf/lakesnes` (MIT).
</content>
</invoke>
