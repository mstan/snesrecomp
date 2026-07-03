# LLE scheduler — path to "rich cfg × LLE" (compiled tasks, real scheduler, no HLE)

Status 2026-07-02. Owner direction: **every game moves to the LLE scheduler tier with a
rich (fully compiled) cfg; the C-host HLE scheduler and its per-game task-PC tables are
retired.** This doc records where each configuration stands, why compiled task bodies
don't yet run under LLE, and the fiber-free design that closes the gap.

## Where each configuration stands

| game | cfg | scheduler | tasks run as | state |
|------|-----|-----------|--------------|-------|
| MMX USA | rich | HLE (default) | compiled (fibers) | shipped, validated |
| MMX USA | rich | LLE (`SNESRECOMP_MMX_SCHED_LLE=1` + immediate ports) | **interpreted** | clean + full speed as of engine `22734b2` |
| Rockman X JP | minimal | LLE (default) | interpreted | shipped (experimental) |
| target | rich | LLE | **compiled via bounce** | NOT YET — this doc |

The 2026-07-02 campaign proved the LLE tier itself is sound: LLE-vs-HLE guest video
state is bit-identical for 4000 frames headless, and the live wedge/garble/chug was
three APU-domain wall-time defects (torn word port stores, per-opcode lock contention,
deferred-port stalls), all fixed in `22734b2` — **not** a scheduler-logic or cfg-richness
problem. See memory/commit messages for the mechanism details.

## Why tasks still interpret under LLE (the bounce suppression)

`_interp_run_core` (interp_bridge.c) in yield mode (`yield_pc != 0`, i.e. the
`interp_bridge_run_scheduler` frame) never bounces a JSR/JSL to a compiled body:

1. The scheduler's yield primitive (`$00:8100` family) is a **coroutine switch**: it
   consumes the caller's return frame, stores a resume PC into the slot's `$32/$33`,
   saves S, restores the scheduler loop's S, and BRAs back into the loop — it never
   returns to its caller. Bouncing any body that transitively reaches it under the
   paired-call ABI (which assumes the callee returns) corrupts the stack.
2. Under HLE those sites are `hle_func`'d to stubs (`HleMmxYieldOneFrame` /
   `HleMmxYieldNFrames` / `HleMmxYieldVblank` / `HleMmxTaskDie`) that suspend the
   hosting **fiber** (`mmx_host_yield` → `SwitchToFiber`). Under LLE there are no
   fibers — a bounced compiled body reaching those stubs would suspend a fiber that
   does not exist.

So today the whole scheduler frame interprets. Correct, but it ships an interpreter for
the game's entire per-frame logic — explicitly rejected for production. Compiled bodies
are the point of the recompiler.

## Design: fiber-free yield via guest-state effect + host NLR unwind

The scheduler's own WRAM state machine already contains everything a suspended task
needs (`$30+X` state, `$31+X` countdown, `$32/$33+X` resume PC, `$36/$37+X` saved S).
The real loop rebuilds task execution from WRAM alone every frame. Exploit that:

**1. LLE-aware yield stubs.** The yield `hle_func` stubs detect LLE context (no fiber /
a runtime flag set by `interp_bridge_run_scheduler`) and, instead of `SwitchToFiber`,
perform the *guest-state effects of the real ROM routine, byte-exact* (derive from the
disassembly of `$8100/$810C/$8121/$80F8`, NOT from the current HLE approximations):

- pop the caller's return frame from the guest stack (resume PC = frame + 1, exactly
  as `mmx_host_yield`'s `MmxSlotResume` capture computes it today);
- write slot state/countdown and the resume PC into the slot's WRAM record;
- save/restore S exactly as the coroutine switch does (task S → `$36/$37`+X analog,
  scheduler S restored).

**2. NLR unwind back to the bridge.** The stub then unwinds the host C stack from
however deep the compiled body is, back to the interp bounce site, using the existing
non-local-return machinery (`RecompReturn` SKIP propagation — the same path
`cpu_unresolved_abandon_balanced` and the save-state resume stop-PC intercepts use).
The bounce site already handles `_air != RECOMP_RETURN_NORMAL` by ending the interp
frame cleanly. The interpreted loop then continues its slot walk natively — the guest
scheduler is none the wiser: from WRAM it looks exactly like the real coroutine ran.

**3. Resume next frame is already native.** The interpreted loop's own dispatch
(`JMP ($0032,X)`) lands mid-task at the recorded resume PC next frame; the interp
executes from there and bounces subsequent calls. Mid-function entry under interp with
scheduler-restored S is proven machinery — it is exactly what
`interp_bridge_resume_task` does for save-state resume today.

**4. Lift the blanket bounce suppression.** With yield sites resolving (via
`cpu_dispatch`) to LLE-aware stubs, yield-mode bounce can be enabled for ordinary
targets. Keep a small cfg-derived denylist for any *other* non-returning coroutine
machinery (the existing `hle_dispatch`/stop-PC set) instead of suppressing everything.

**No fibers. No per-game task table.** The only per-game knowledge is what the rich cfg
already encodes: which PCs are the yield/die primitives (`hle_func` directives that
minimal cfgs need anyway).

## Validation plan (the differential bonus)

Rich-LLE-bounced vs rich-LLE-interpreted is a **whole-game codegen differential**: both
sides run the identical guest instruction stream under the identical scheduler, one
executing task bodies compiled, the other interpreted. Under the co-sim shared APU
clock (`SNES_COSIM_APU_SHARED=1`) any cpu/ram divergence is a recompiler-semantics bug,
drillable to the exact opcode with `SNES_COSIM_SYNC_PC`. Gates:

1. A-vs-A determinism (bounced) — 0 divergence.
2. Bounced vs interpreted — cpu/ram/ppu bit-exact over title + attract + demo.
3. Live: full-speed, clean render, multiple restarts (the wall-time race class).
4. Then flip defaults per variant (`MMX_SCHED_LLE_DEFAULT=1` + immediate ports
   becomes the build default) — USER SIGN-OFF required before changing USA's shipped
   default; HLE stays available via env until removal is approved.

## Also required for "LLE everywhere"

- **Immediate APU ports under LLE** (deferred scheduling stalls interp'd handshakes —
  the JP gate-#3 / USA copyright-freeze class). When LLE becomes a build's default,
  `SNESRECOMP_APU_IMMEDIATE_PORTS_DEFAULT=1` goes with it. The SMW missed-SFX deferred
  path remains the default for HLE builds only.
- **JP cfg enrichment**: JP is minimal; grow it toward rich with the tier-2 gap
  manifest + `tools/tier2_ingest.py` worklist once bounce is enabled (bounce is what
  makes the manifest see un-compiled task callees as tier-downs).
- **Audio validation** (issue #4 lineage) is still the open acceptance bar per variant.
- SMW / Zelda / SM adopt the same pattern when their scheduler idioms are LLE'd
  (SM's single-fiber WaitForNMI HLE is the analogous seam).
