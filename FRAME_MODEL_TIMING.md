# Frame-model interrupt timing (#3) — design + scope

**Status: scoped, not yet implemented. The deepest faithfulness lever; the
unifying root of the remaining MMX guest-state residual AND a candidate for the
audio "occasional off-pitch note".**

## The problem

The recomp runs each frame as **discrete, sequenced host-C calls**:

```
RtlRunFrame:
  run_frame()      = I_NMI()  then  scheduler (MmxSchedulerTick HLE / $8099 LLE)
  draw_ppu_frame() = PPU line render + HDMA + raster-IRQ sim (I_IRQ at scanline vTimer)
```

Real hardware interleaves these **continuously**: the CPU executes instructions
non-stop; NMI fires at vblank start (~scanline 225) *interrupting whatever
instruction boundary it lands on*; the raster IRQ fires at scanline `vTimer`
mid-frame, likewise. The recomp instead runs the whole CPU frame, *then* the
whole render/IRQ pass — so an interrupt's effects land at a different point in
the guest's execution than on hardware.

## Evidence it's the residual root

- **MMX co-sim:** HLE and LLE agree for 156 frames, then fork at frame 157 (the
  attract-demo transition) on the scheduler slot state ($0031) + downstream
  ($02EE DMA bookkeeping). Neither HLE nor LLE converges to bsnes past there.
  The fork is *timing-sensitive* (accumulated micro-differences in when the
  scheduler runs relative to NMI/IRQ), not an arithmetic bug — the byte-exact
  HLE delay-countdown fix (56e4aba) did not move it.
- **Audio (candidate):** the SPC sequence data is byte-identical to bsnes, so the
  sound *commands* match; but the *timing* of CPU→SPC port writes within the
  frame depends on when the CPU runs relative to interrupts. A faithful interrupt
  model is the plausible lever for the "occasional off-pitch note" (issue #4's
  second symptom) — the uniform pitch is already proven clean.

## The execution-model constraint (shapes the whole design)

- The **compiled** recomp has **no continuous instruction stream** — it is host-C
  functions with block-granular `master_cycles`. You cannot "fire an interrupt
  between instructions" cleanly; there is no instruction boundary to land on.
- The **interp816 (LLE) path DOES** have an instruction stream and per-opcode
  `cyclesUsed`. Interrupt timing is therefore **feasible in the LLE/interp path**
  and not (cheaply) in the compiled path.

This is why #3 builds on the LLE tier: the LLE is the vehicle for cycle-accurate
interrupt timing.

## Design options

**A. Cycle-accurate interrupts in the LLE path (recommended).**
While interpreting a frame, accumulate opcode cycles into a scanline/H-V position
counter. Fire I_NMI at the vblank-start cycle and I_IRQ at the `vTimer` scanline
cycle, at the *actual* opcode boundary reached — exactly as hardware. This makes
the LLE fully faithful (should close the frame-157 fork) and turns it into the
true accuracy reference. Requires:
  - a master-cycle→scanline model in the interp frame loop (the LakeSnes
    `handle_pos_stuff` H/V counter ported into ref_driver.c already does this for
    the Track-A ref — reuse it);
  - firing I_NMI/I_IRQ mid-interpretation at the right cycle, pushing the real
    interrupt frame (cpu_push_interrupt_frame) and vectoring the interp;
  - the scheduler LLE (interp_bridge_run_scheduler) becoming a *consequence* of
    the CPU spinning on $0B9D and NMI firing, rather than an explicit per-frame
    call — i.e. the whole frame is one interpreted stream with interrupts.

**B. Full interrupt-driven compiled recomp.** Fire interrupts between compiled
blocks (at WatchdogCheck/block boundaries) using `master_cycles` to decide when.
Coarser (block-granular, not opcode) and invasive across all generated code.
Lower fidelity than A, much larger blast radius. Not recommended as the first
step.

**C. Hybrid: LLE cycle model drives compiled-HLE interrupt phase.** Use the
interp's cycle/scanline model to decide *when* the compiled HLE fires I_NMI/I_IRQ
each frame (phase within the frame), without going fully interrupt-driven. A
cheaper partial improvement to the shipping HLE; a possible interim.

## Recommended path

1. **Prototype A on the co-sim LLE** (dev-only, no production risk): add the
   scanline/H-V cycle model to the interp frame loop, fire NMI/IRQ at cycle-true
   points, and measure whether the frame-157 MMX fork closes and WRAM agreement
   vs bsnes rises past 98.3%.
2. If A closes it, the LLE becomes the cycle-accurate reference. Then evaluate
   **C** (drive the shipping HLE's interrupt phase from the same cycle model) for
   a cheap production fidelity bump.
3. Re-run the **audio** DSP-output co-sim under A to test the occasional-note
   hypothesis (CPU→SPC port-write timing).

## Validation (co-sim, as built)

- MMX WRAM align vs bsnes: does the frame-157 fork close / agreement exceed 98.3%?
- Gate-1 determinism in both HLE and LLE modes.
- SMW regression check (shared frame model).
- Audio: DSP-output spectral + (new) occasional-note windowed diff.

## Size

Medium-large. Option A is a focused change confined to the interp/LLE frame loop
(dev/accuracy tier), reusing the ref_driver H/V model — but it is real
interrupt-model work and should be its own focused session, not bolted onto
unrelated changes.
