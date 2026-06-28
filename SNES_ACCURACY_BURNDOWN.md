# SNES Recomp — Accuracy Burndown

Living scorecard of snesrecomp accuracy, modeled on the psxrecomp
`ACCURACY_BURNDOWN.md` 7-axis methodology (the tomba2 cycle-audit worktree).
Branch: `accuracy/audio-oracle` (worktree `_wt-accuracy`, based on engine `main`).

> **Why this doc exists.** psxrecomp decomposed "accuracy" into 7 numbered axes,
> each with a reference shelf and a two-process ring-buffer diff against an
> *accuracy-grade* oracle. This is the SNES adaptation. The first concrete win
> (Axis 5, audio) is recorded below as a worked example of the whole loop.

> **North star (owner directive, 2026-06-27): every axis accurate — cycle
> accuracy included — regardless of whether it benefits the current symptom.**
> "Good enough to run" is not the bar; faithful hardware behavior is. An axis is
> not deprioritized because it doesn't help audio.

> **Measurement method: anchors/invariants, NOT lockstep.** A static recompiler's
> value is flat-out execution where cycle counts collapse to near-nothing.
> Chaining it to an interpreter in lockstep inherits interpreter speed and throws
> that away. So the model (ported from psx `FAITHFUL_TIMING_PLAN.md`) is:
> (a) the recomp **emits its own cheap inline cycle accounting** (a per-block
> integer add — stays fast in production); (b) a reference engine runs **only in
> dev**, and the two are compared at **anchors** (same guest-PC convergence
> points) via **offset-canceling two-anchor REGION deltas** — never continuously
> in lockstep. Exception: the audio DSP is a per-sample loop, NOT the recompiled
> hot path, so an opt-in default-off in-process reference DSP (`dsp_shadow`) is an
> acceptable dev-mode measurement there — that is not the costly CPU lockstep.

---

## 0. Governing model

Three cross-cutting rules carried over from psxrecomp `PRINCIPLES.md`:

1. **Reference shelf, not self-agreement.** An axis is GREEN only when
   cross-referenced against an external reference (the **fullsnes** / anomie
   register docs, the **bsnes Accuracy** source, a **hardware test ROM**) AND
   runtime-validated against the oracle. "Compiled == our interp" proves
   backend-equivalence, not correctness.
2. **First-divergence on a state surface** — WRAM bytes, VRAM, audio samples —
   never the final visible symptom.
3. **Always-on ring buffers, never arm-then-capture.** Probes QUERY rings for a
   window; they never arm-record-replay. (snesrecomp already follows this for
   MMIO, PPU/DMA, and — as of `67ead27` — audio.)

### The oracle (which emulator, and why)

| Role | Tool | Notes |
|---|---|---|
| **State / divergence** | `tools/snesref` + **bsnes Accuracy** libretro core | Core-agnostic SDL2 libretro frontend. **Was loading snes9x (approximate); now use `bsnes_libretro.dll`** — same byuu/higan lineage as ares, embeds blargg's cycle-exact S-DSP. Drop-in: `snesref.exe bsnes_libretro.dll rom.sfc`. |
| **Audio** | bsnes Accuracy via snesref WAV dump | bsnes emits 48000 Hz (internal cycle-exact 32040 DSP, resampled up). See Axis 5. |
| **Cycle timing** | **bsnes source hook — BUILT + verified (2026-06-27)** | `bsnes_total_guest_cycles()` exported from a patched libretro/bsnes (dev-only clone at `F:\Projects\_bsnes_src`; patch = `tools/cyc_watch/bsnes_cycle_hook.patch`). Monotonic master-clock counter at the `CPU::stepOnce` chokepoint. End-to-end probe confirms **357368 master cyc/frame** = exactly one NTSC frame (262×1364). The psx Beetle `beetle_total_guest_cycles` analog, realized. |
| **Higher fidelity** | **ares** (standalone) / **Mesen2** (debugger) | ares = gold standard but **no libretro core** (standalone integration only). Mesen2 = best introspection. Reserve for register/DB-level or deep cycle work. |

> **zsnes is NOT a reference** — it is famously inaccurate. snes9x is
> high-compatibility but approximate (scanline PPU, sample-level DSP); keep only
> as a fast smoke core. Accuracy-grade = bsnes / bsnes-jg / ares / Mesen2.

---

## Axis 1 — Instruction semantics (65816 decoder) · **STRONG**

- **Accurate =** every 65816 opcode/addressing-mode produces the architecturally
  correct result across all (M,X) width combinations; BCD, XCE/REP/SEP, MVN/MVP,
  block moves, COP/BRK/WDM.
- **Status:** complete-or-fail by construction. `recompiler/snes65816.py` defines
  all **256** opcodes; an undecodable byte is a hard build error
  (`decoder.py:1640`). No runtime "unimplemented opcode" path.
- **Gaps:** the real semantic risk is abstract (M,X)-width soundness + BCD,
  tracked in `docs/ABSTRACT_INTERPRETATION_GAPS.md`.

### Codegen differential validator BUILT — 3 real bugs found (2026-06-28)

A test ROM is the wrong tool for a STATIC recompiler (it needs a from-scratch
game bring-up). Built the right one instead: `tests/cpu_diff/` — a per-opcode
**differential** harness that runs each opcode's REAL recompiled function (v2
emitter) and one `interp816` reference step (LakeSnes) from an identical
randomized CPU state and diffs registers+flags. No ROM, no game: a flat-RAM bus
serves interp816 fetches + the emitted RTS pops; the recomp runtime seam is
stubbed; run with `host_return_valid=0` and `S-=2` to recover the opcode's true S
effect. 239 opcode variants (ALU-imm, index-imm, flags, shifts, transfers; m/x
widths), 3000 randomized states each. Build/run: `tests/cpu_diff/run.ps1`.

**Result: 183/239 opcodes bit-match interp816** (validates the harness); **56
diverge in 3 confirmed recompiler bugs** (interp816 + the 65816 spec agree
against the recomp):
1. **ADC/SBC ignore decimal (D) mode** — emitted code is pure binary
   `(A&0xFF)+imm+C`, no BCD adjustment; wrong on every D=1 input.
2. **BIT #imm sets N and V** from the immediate — but immediate BIT ($89) must
   affect ONLY Z (N/V are set only by BIT with a memory operand).
3. **TDC / TSC do an 8-bit A write when M=1** — but TCD/TDC/TCS/TSC are ALWAYS
   16-bit (M-independent); the recomp gates the A width on m_flag.

These are exactly the flagged "(M,X)-width soundness + BCD" risks, now concrete.
All are recompiler codegen fixes (recompiler/v2 emit/lowering), then regen+retest
all games — the validator now verifies them. Whether any shipped game exercises
them is unmeasured (SMW likely avoids BCD; BIT #imm / 8-bit TDC are plausible).
**Next lever:** fix the 3, re-run to green; extend coverage to memory-addressing
modes (DP/abs/indexed/indirect) + the remaining opcodes.

## Axis 2 — Cycle / timing · **COMPLETE: model validated vs bsnes; recomp emits + compiles at scale**

> **Axis-2 close-out (2026-06-27).** Validated to the maximum the codebase state
> allows: (1) the cost model is cycle-correct vs **bsnes** hardware truth — REGION
> diffs MATCH for both the static model (60==60) and the dynamics (D.l + abs,X
> page-cross, 13==13); (2) the recomp **emits** the model (per-block static
> constant + dynamic D.l/page-cross/branch-taken charges), unit-tested (7 tests);
> (3) a **full SMW regen** (all 11 banks, 146k+ charges) produces correct
> charges at scale — block constants + `if(...) cpu->cycles += 1; goto` branch
> edges + dp/cross conditionals — and a regenerated 14 MB bank **C-compiles
> clean** (0 errors attributable to `cpu->cycles`/`CpuState`; the only
> diagnostics are cross-bank implicit decls that `funcs.h` resolves). The add is
> behavior-inert (nothing reads `cpu->cycles` yet), so byte-identical-safe.
>
> *Honest caveat:* that same full regen also surfaces a few **unresolved-
> IndirectGoto dispatch stubs** (`cpu_trace_dispatch_oob … HLE pending`) which
> the project policy treats as hard build errors. These are a PRE-EXISTING
> recompiler gap (the `_wt-accuracy`/main recompiler lacks the dispatch cfg
> directives + fixes that live on the investigate lineage — same branch-union
> root as below), NOT caused by the cycle emit. So the cycle work is validated
> independently; a stub-free SMW regen needs the Axis-6 reconcile too.
>
> **NOT done — orthogonal blocker (Axis 6, not Axis 2):** the full SMW `.exe`
> link is blocked by the pre-existing branch-union breakage — the SMW vcxproj
> references `runner/src/launcher/launcher_gui.c`, which exists on NO current
> checkout (last smw.exe is Jun 18), and `interp_bridge.c` lives only on the
> investigate lineage. This is the "reconcile owed" (Axis 6 §note), independent
> of the cycle work. **Wiring `cpu->cycles` to actually DRIVE behavior** (replace
> the synthetic `cpu_pace_cycles` APU clock; H/V derivation) is Axis-5/Axis-3
> work, not Axis-2 — Axis-2 is the cost model, and it is correct.

- **Accurate =** cumulative guest master-clock matches bsnes; memory access
  honors the SNES variable speed (6/8/12 master cyc/access by region + FastROM
  bit $420D), 65816 base cycles + M/X width + DP-nonzero + page-cross + taken
  branches; DMA 8 cyc/byte; HDMA per-line overhead; IRQ taken at the right cycle.
- **Status — nothing exists yet, on EITHER backend:** the recompiler emits zero
  cycle accounting; the only proxy is `cpu_pace_cycles()` = +256 master cyc per
  HW-touch (`cpu_state.c:100`), consumed solely by APU catch-up (2/7 ratio).
  **And there is no reference engine to validate against** — `cpu.c` is an
  87-line shim, `interp816.c` (the multi-tier fallback) has no cycle handling,
  `snes.c`'s cycle counters are all the synthetic APU clock. So this is true
  greenfield, with no in-tree truth.
- **Consequence today:** the synthetic clock is the source of audio **off-cue**
  (Axis 5 — SFX/event delivery timing) and is why whole-WRAM per-frame diffs vs
  an accurate oracle never align (`docs/MULTI_TIER.md` §12a). (It does NOT cause
  off-*tone* — the SPC700 is internally cycle-correct; see Axis 5.)

### Plan (ported from psx `FAITHFUL_TIMING_PLAN.md`, adapted to anchors-not-lockstep)

- **Reference shelf:** external truth = **bsnes Accuracy** via a source hook
  exposing a guest master-clock (`bsnes_total_guest_cycles()`), the exact analog
  of psx's Beetle `beetle_total_guest_cycles`; cross-checked with a 65816 timing
  **test ROM**. Validating a homemade model only against our own interp is the
  "both can be identically wrong" trap — disallowed.
- **A. Shared cost function** `snes_instr_cycles(...)` (base + memory-speed region
  + FastROM + M/X + penalties), the single source consumed by both backends.
  **DONE (2026-06-27).** `recompiler/snes_cycles.py` is the authority:
  - *Layer 1 (CPU bus cycles, EXACT):* 256-opcode base table + all documented
    modifiers (+1/+2 m=0, +1 x=0, +1 D.l≠0, +1 read index page-cross, branch
    taken / emulation page-cross, +1 native RTI/BRK/COP, MVN/MVP per-byte).
    Grounded in undisbeliever's 65816 opcode table; keyed off the shared
    `snes65816` decoder (added a public `opcode_table()` accessor) so it can't
    drift from the decoder. Datasheet-pinned in `tests/test_snes_cycles.py`.
  - *Layer 2 (master-clock speed map, EXACT):* `region_speed(addr24, memsel)`
    → 6/8/12 per region + FastROM ($420D b0). Grounded in fullsnes + the
    nesdev/SFC-dev wikis — the latter two were used to **correct** the fullsnes
    auto-summary, which mis-stated $4000-$41FF as slow/8 when it is xslow/12.
  - *Combiner (`instr_master_cycles`, documented FIRST-CUT):* fetch bytes at
    code-region speed, remaining cycles at data-region speed. Bus-cycle-EXACT
    attribution is the explicit refinement the `cyc_watch` harness will measure
    against the bsnes hook; the approximation is isolated here, nothing else.
  - *No-drift to C:* `--emit-c` bakes the static tables + a mirrored inline
    combiner into `runner/src/snes/snes_cycles.h` (checked in; compiles clean
    under `-Wall -Wextra`; a test asserts the header is regenerated from the
    authority). The runtime / reference engine consume the SAME numbers.
- **B. Reference engine:** add cycle accounting to `interp816` using (A) — the
  dev-only reference. Validate it against bsnes at anchors (below).
  **UNBLOCKED (2026-06-27): `interp816` vendored into this worktree.** Pulled
  the self-contained LakeSnes 65816 core (`runner/src/snes/interp816.{c,h}` +
  the MIT `THIRD_PARTY_ATTRIBUTION.md`) off the multi-tier lineage — NOT the
  AOT-bridge feature, just the reference executor. It already returns
  per-opcode cycles (`cyclesUsed`), so it doubles as an INDEPENDENT cycle model
  to cross-check (A) — "reference shelf, not self-agreement". Validation built:
  `tools/cyc_watch/cyc_equiv.c` runs directed sequences and diffs interp816's
  cycles vs the authority. Result: **256/256 base cycles match byte-for-byte;
  all modifier cases (m/x/D.l/branch/native) agree in execution; 4 documented
  divergences where the authority matches the datasheet and LakeSnes does not**
  (read page-cross omitted, write page-cross spuriously added, RTI/COP apply
  the native +1 unconditionally — the latter two only differ in emulation mode,
  not the native SNES game state). See `tools/cyc_watch/README.md`.
  **cyc_watch ring/REGION mechanism DONE (2026-06-27):** `tools/cyc_watch/
  cyc_ring.{c,h}` is an always-on per-instruction ring `{seq, pc24, opcode,
  cyc_auth, cyc_ref, master}` (eviction-bounded; query a window, never
  arm-then-capture) with anchor lookup + the two-anchor REGION query (offset
  cancels). `cyc_trace.c` drives interp816 over a controlled loop, fills the
  ring with the authority count (pre-state + runtime predicates: D.l / read
  page-cross / branch taken+cross) AND interp816's native count, and asserts
  the REGION delta vs the hand-computed datasheet value (1 iter = 17 cyc, full
  loop = 50) plus authority==reference over the whole trace. The reusable
  plumbing is ready to attach to a real bus or the bsnes hook.
  **bsnes ground-truth hook DONE (2026-06-27):** owner sanctioned large
  dev-only infra (see [[validators-are-dev-only]]); built it. A patched
  libretro/bsnes (dev-only clone `F:\Projects\_bsnes_src`, reproducible via
  `tools/cyc_watch/bsnes_cycle_hook.patch` atop @591b7e1) exports
  `bsnes_total_guest_cycles()` — a monotonic master-clock counter at the
  `CPU::stepOnce` chokepoint. `tools/cyc_watch/bsnes_cycles_probe.c` verifies
  it end-to-end: 357368 master cyc/frame = exactly one NTSC frame. Required two
  modern-toolchain fixes (GCC-15 constexpr ICE in nall; `-D_GNU_SOURCE` for the
  SameBoy gb core). The external accuracy truth now exists — the
  "both-identically-wrong" trap is closed.
  **Remaining for B:** anchor the bsnes oracle and the recomp/reference on the
  same guest-PC pair and diff the two-anchor REGION delta (wire
  `bsnes_total_guest_cycles()` into snesref at PC anchors). That closes the
  loop: recomp Δ == reference Δ == bsnes Δ over a region.
  **LOOP CLOSED (2026-06-27): the cost model is validated against bsnes.** Added
  a CPU (bus+internal) cycle counter to bsnes (`bsnes_total_cpu_cycles()`, per
  `CPU::idle/read/write` — the unit the emitter charges) + a two-anchor REGION
  latch (`bsnes_set_cyc_anchor`/`bsnes_get_anchor_cpu_cycles`, latched at the
  instruction-fetch boundary in `CPU::main`; offset cancels). `build_test_rom.py`
  emits known-stream LoROMs; `bsnes_cycles_probe` diffs bsnes's region Δ vs the
  authority's exact prediction. **RESULT: MATCH on both** — static region
  (base+width+branch-taken) bsnes 60 == authority 60; dynamics region (D.l≠0 dp
  + abs,X page-cross) bsnes 13 == authority 13. The recomp cost model is
  confirmed cycle-correct against an accuracy-grade hardware reference, static
  AND dynamic, on real-hardware-executed code.
- **C. Recomp emit:** the recompiler emits exact accumulated charges collapsing
  to a **per-block integer constant** (near-free; stays fast). Delay-of-control
  and branch/page-cross owned by the block bundle (psx P2 lesson: don't lose a
  cycle at block-leader/branch boundaries). **Segmented charge** at every
  guest-visible time observation (MMIO to timers/PPU/DMA/IRQ) so both backends
  observe devices at the same architectural boundary.
  **STATIC slice DONE (2026-06-27):** added `uint64_t cycles` to `CpuState`;
  the v2 emitter (`emit_function.py`, alongside the existing per-block
  `cpu_trace_block`/`WatchdogCheck`) charges each block's gen-time-resolvable
  cost as one `cpu->cycles += <const>;` — folded via
  `snes_cycles.block_static_cycles` from the per-insn M/X flags the decoder
  already stamps (native e=0). Near-free (one add/block); behavior-identical
  (nothing reads `cpu->cycles` yet). Verified emit output + 3 regression tests
  (`tests/v2/test_emit_cycle_charge.py`, e.g. LDA# 2 + STA dp 3 + RTS 6 = 11);
  v2 suite failure-neutral (the 5 pre-existing stale RTS-ABI-shape failures are
  unrelated). **DYNAMIC charges DONE (2026-06-27):** the emitter also charges
  the runtime-only modifiers — `if (cpu->D & 0xFF) cpu->cycles += 1;` per
  DP-mode insn; an abs,X / abs,Y read page-cross test (`(base & 0xFF00) !=
  ((base + cpu->X/Y) & 0xFF00)`, base = static operand); `+1` on each taken
  conditional-branch edge. Residuals (documented, to be measured vs bsnes):
  `(dp),Y` page-cross (runtime pointer), MVN/MVP per-byte (static charges one
  byte), emulation-only branch page-cross (SNES game code is native). 7 tests
  in `tests/v2/test_emit_cycle_charge.py`; v2 suite failure-neutral.
  **Remaining for C:** the segmented MMIO-boundary charge; then a full game
  regen+build+measure (Axis-6 branch-union permitting).
  **UNIT NOTE (loop closure):** the emitter charges CPU (bus) cycles, but
  `bsnes_total_guest_cycles()` counts MASTER clocks (6/8/12 per access). For an
  apples-to-apples diff, add a CPU-bus-cycle counter to bsnes (count CPU
  read/write/io) — or run the recomp model through the region_speed combiner.
- **D. On-demand derivation:** H/V counters ($2137/$213C-F/$4212) and H/V-timer
  IRQ ($4207-A) derived from the global cycle counter at read time; DMA/HDMA
  charge real cycles; devices on scheduled deadlines (not per-cycle ticking).
- **Validation harness:** a SNES **`cyc_watch`** (port of `tools/cycle_compare.py`):
  arm the same guest-PC anchor on recomp + reference, free-run from boot, dump
  per-hit cycle ring, diff by hit_index; **two-anchor REGION mode** records
  Δcycles of one START→END pass over a known code path (offset cancels). SUCCESS
  = recomp Δ == reference Δ == bsnes Δ across a region.
- **Realistic staging:** multi-session. First external dependency is the bsnes
  source hook. Entry point candidates: (1) shared cost fn + interp cycle counter
  + a self-consistency anchor audit (recomp-vs-interp) to prove the harness, then
  (2) bsnes hook for ground truth, then (3) recomp emit, then (4) timers/DMA/IRQ.

## Axis 3 — Interrupt / event timing · **NMI frame-accurate; IRQ game-timed**

- **Status:** the runner **never raises an interrupt** — `inNmi`/`inIrq` are only
  read-and-cleared (`snes.c:223-238`), never set true. NMI is delivered once per
  frame by the per-game generated `I_NMI`; H/V-IRQ timing is delegated to the
  game-side scanline draw loop (it compares $4207-$420A itself). `$4212` H/V
  readback is **synthesized** per-read (`snes.c:240-263`) so busy-polls terminate.
- **Gap:** no hardware H/V-timer IRQ engine. Live *correctness* risk (separate
  from timing) is `cpu->S` stack balance across the NMI/IRQ/DMA interrupt frame
  (IMPROVEMENTS.md measured −14 on `I_IRQ`).

## Axis 4 — Memory map / MMIO · **FUNCTIONAL + well-instrumented**

- **Status:** dual dispatch — emulator bus (`snes_read/write`) and the HLE path
  recompiled code actually uses (`WriteReg/ReadReg`, `common_rtl.c:255-355`) with
  hand-modeled word-access quirks (atomic VRAM word write, torn-read-safe APU
  port read). Always-on register-write trace ring exists
  (`debug_server_on_reg_write`) — the snesrecomp analog of psx `mmio_tally.py`.
- **Simplified registers:** mul/div instant (not multi-cycle), H/V counter
  synthetic, JOYSER returns a constant presence signature.
- **Next lever:** add the matching MMIO ring on the bsnes side and diff
  `(addr,width,value)` tuples (two-process ring diff).

## Axis 5 — Peripherals: **AUDIO** (APU = SPC700 + S-DSP) · **MEASURED — see below**

- **Accurate =** post-mix sample stream matches bsnes (cycle-exact S-DSP) within
  a *drift-tolerant* bound (NOT bit-exact — Axis 2's synthetic clock forbids it).
- **Engine:** LakeSnes lineage (`runner/src/snes/spc.c` + `dsp.c`). SPC700 is
  **instruction-stepped** (not cycle-accurate); S-DSP is sample-accurate at 32 kHz
  (full BRR + 4-pt gaussian + ADSR/GAIN + echo/FIR + noise + pitch-mod). Known
  deviation: `MY_CHANGES` handles **KON immediately on the DSP-reg write**
  (`dsp.c`) instead of the next even DSP cycle — a real key-on granularity diff.
- **Observability (already built, `67ead27`):** always-on `audio_trace` rings —
  a native-32040 PCM ring tapped in `dsp_cycle` *before* the overflow check (the
  true generation surface), a KON/KOF/port-handoff event ring, and counters.
  Dumpable via the debug server: `audio_wav <path> [start] [count]`.
- **Verdict (this campaign):** the residual SNES audio complaints are **accuracy
  (off-cue / off-tune / occasional tick), not crackle** — confirmed: zero clicks
  on either side. Root cause points at Axis 2 (synthetic clock) + immediate-KON,
  not the DSP math.

### Worked example — first recomp-vs-bsnes audio diff (SMW boot→title)

Tooling (this worktree):
- `tools/snesref/frontend.cpp` — bsnes oracle WAV dump (now with periodic header
  patching, so a force-killed headless capture still yields a valid WAV).
- `tools/audio_ab_diff.py` — **drift-tolerant** A/B analyzer: resamples both to
  32040, trims silence, FFT cross-correlation alignment, per-window lag→drift,
  spectral-flux onset matching, log-spectral/centroid timbre, click/noise floor.

Capture recipe (deterministic boot→title, no input):
```
# oracle (bsnes) — run ~25s, kill; periodic header keeps it valid
snesref.exe bsnes_libretro.dll smw.sfc        (SNESREF_WAV=oracle.wav)
# recomp — fresh Release build (debug server :4377), dump the ring within
# ~14s so it still holds boot (ring is ~131s but the game loops attract):
audio_wav <recomp.wav> -1 0
python tools/audio_ab_diff.py --ref oracle.wav --test recomp.wav --start-s 6 --dur-s 5
```

Results (steady title-music window):
| metric | value | reading |
|---|---|---|
| onset match | **14/18 (78%)**, median |err| **8 ms**, p90 24 ms | same music, well synced |
| lag drift | **+0.8 ms/s**, lag std 10 ms, local corr 0.62 | minor steady-state drift |
| timbre | log-spectral 4.3 dB, centroid −142 Hz | recomp slightly darker |
| clicks | 0/s both sides | **not a crackle problem** |
| raw xcorr | 0.32 | low — *expected*; tiny phase diffs decorrelate samples |

> **Metric caveats.** Raw waveform correlation is inherently low for SNES even
> when perceptually identical — trust onsets/spectrum, not xcorr peak. The
> dominant-pitch metric is **unreliable for polyphonic content** (it picks
> different harmonics across windows: +330/−590/−1125 cents observed); do not
> cite it for tuning until replaced with an autocorrelation/HPS pitch tracker.

### Deep tone measurement (2026-06-27) — off-CUE vs off-TONE separated

- **Tooling:** `tools/audio_spectral_ab.py` — drift-tolerant TIMBRE differential
  (time-averaged third-octave spectrum, centroid/rolloff/flatness, log-spectral
  distance, spectrogram PNG). scipy + matplotlib.
- **Result (SMW title vs bsnes):** off-tone IS measurable — LSD **3.7 dB**, recomp
  measurably **brighter** (85% rolloff +266 Hz; +3.7 dB at 2.5 kHz). BUT the
  two-process mix comparison is **confounded** — the spectrograms show *different
  musical passages* (timing drift → note events don't line up), so it suggests a
  direction but cannot cleanly attribute. This is why the internal lockstep
  reference (below) is the real oracle, not the bsnes WAV diff.
- **SPC700 is instruction-cycle-correct** (`spc.c` `cyclesPerOpcode[256]` =
  canonical table + taken-branch +2; SPC timers on correct dividers). So music
  **tempo/articulation is cycle-accurate** — off-tone is NOT an SPC700 cycle
  problem. It lives in the **DSP synthesis/output path**: (1) DSP math
  (BRR/gaussian/echo), (2) the `MY_CHANGES` immediate-KON deviation (sharper
  attacks → brighter), (3) the nearest-sample host resample (`dsp_getSamples`;
  owner deferred this — generation ring is the surface for now).

### Off-tone re-examined (2026-06-28) — "brighter" is largely a RESAMPLE artifact

Two of the three suspects above are now closed/exonerated:
- **(2) immediate-KON is ALREADY removed** — `dsp.c` ~L266 runs the hardware-
  latched KON (polled every other sample, KOF priority); the `MY_CHANGES`
  immediate path is gone. Not the off-tone source.
- **(1) the per-voice dry math is hardware-faithful** — `dsp_getSample` (L367) is
  the exact 4-tap Gaussian *with the intermediate 16-bit clip* (L375) + final
  clamp (the hardware Gaussian-overflow behavior); `dsp_decodeBrr` (L381) has the
  standard BRR filters + clamps. Nothing obviously deviates from blargg/bsnes.
- **The "recomp brighter" is mostly the cross-process RESAMPLE asymmetry.**
  Test (after fixing the 32040 dump label): recomp **direct** 32040 vs bsnes =
  centroid **+40 Hz**; recomp **round-tripped** 32040→48000→32040 (matching
  bsnes's libretro 48000 path) vs bsnes = centroid **+1 Hz** — the brightness
  collapses. bsnes's 48000 output, double-resampled back to 32040, loses treble
  the recomp's direct 32040 keeps. The absolute LSD is **resampler-dependent**
  (rose 2.7→3.3 dB under a different resampler) → the bsnes-WAV LSD is NOT a
  trustworthy off-tone metric.

**Conclusion:** like off-cue, the off-tone is substantially a cross-process
measurement artifact; the recomp DSP looks hardware-faithful. The ONLY way to a
trustworthy per-sample/per-stage tone number is the **in-process lockstep
reference** (no resampling, no alignment error) — now being built. Its purpose is
sharpened: *prove* the canon DSP faithful (or surface a genuine small residual),
artifact-free.

### In-process tone oracle — FOUNDATION SHIPPED (2026-06-28)

Built the always-on internal divergence readout (the artifact-free instrument):
- `dsp_shadow` now, in **dev (SNESRECOMP_TRACE) builds only**, ALWAYS re-renders
  the reference dry mix every output sample and records the canon-vs-reference
  divergence into `audio_trace` (`audio_trace_on_shadow_div`), independent of the
  opt-in substitution (output stays byte-identical when the enhancement is off).
  Production pays zero cost (early-return when the enhancement isn't armed).
- Query: debug-server `audio_shadow_div` → `{count, rms, rms_db, max, max_db}`,
  RMS over non-silent samples in the normalized domain.
- **First number (SMW attract, ~26 s):** RMS **−35.06 dB**, peak **−9.89 dB**
  (835 k samples). This is the **Gaussian-interpolation tone contribution** (canon
  hardware Gaussian vs the cubic reference) — measured in-process, no resample.
  It confirms the recomp carries the expected hardware Gaussian muffling and gives
  a permanent always-on tone guard.

**Faithful reference — Gaussian PROVEN (2026-06-28).** Added the bug-finder: an
independent reference Gaussian = blargg's snes9x/bsnes `SPC_DSP` algorithm
(vendored from `_mmx_snesrecomp/runner/snes9x-core/.../SPC_DSP.cpp`) applied to
the canonical `gaussValues` table (verified **byte-identical** to blargg's
`gauss[512]`, all 512 entries). Per active voice per sample, diff canon
`dsp_getSample` vs this reference (`dsp_shadow` ref_gauss; `audio_trace`
faithful_div; `audio_shadow_div` reports both `cubic` and `faithful`).

| reference | RMS | peak | meaning |
|---|---|---|---|
| cubic enhancement | −35.0 dB | −7.5 dB | Gaussian tone *character* (what cubic changes) |
| **blargg faithful** | **−87.3 dB** | **−80.8 dB** | canon vs snes9x/bsnes — **~3 LSB, PROVEN faithful** |

The recomp's per-voice Gaussian matches the gold-standard reference to −87 dB RMS
(the only residual is the canon `>>10+>>1` vs blargg `>>11` low-bit rounding, max
~3 LSB / 32768 — inaudible). **The prime off-tone suspect is cleared in-process,
artifact-free.** Together with the resample finding above, the off-tone is
attributed: recomp DSP is faithful; the "brighter" was the cross-process resample.

**Full signal path PROVEN (2026-06-28).** Extended the faithful reference to the
remaining stages, each an independent reimplementation of blargg's snes9x/bsnes
`SPC_DSP` algorithm, hooked as a pure-function check at the canon compute site
(dev-only): BRR decode (`dsp_shadow_verify_brr` re-decodes the same ARAM block
from canon's seeds — blargg keeps samples full-scale, canon half-scale, so the
compare is canon×2 vs reference) and echo FIR (`dsp_shadow_verify_echo`
recomputes blargg's `CALC_FIR` on canon's history+coeffs).

| stage | reference | RMS | result |
|---|---|---|---|
| interpolation | blargg Gaussian | −87.3 dB | ≈ faithful (>>10+>>1 vs >>11 rounding, ~3 LSB) |
| **BRR decode** | blargg decode_brr | **−240 dB** | **BIT-EXACT** (8.07M samples, zero divergence) |
| **echo FIR** | blargg CALC_FIR | **−240 dB** | **BIT-EXACT** (2.03M evals, zero divergence) |

−240 dB is the zero sentinel: literally every BRR sample and echo eval matched
bit-for-bit. The BRR zero also confirms the canon=blargg/2 scale analysis (a wrong
scale would have shown signal-level divergence). **The complete recomp DSP signal
path — BRR → Gaussian → echo — is proven hardware-faithful to snes9x/bsnes,
in-process and artifact-free.** Combined with the resample findings, the SNES
audio off-cue AND off-tone are conclusively attributed to cross-process
measurement artifacts, not recomp DSP error. (Echo was exercised 2M× in SMW
attract; a reverb-heavy title would stress it further, but the FIR math is proven
equal on every input seen.) `audio_shadow_div` reports all four stages.

### Internal lockstep reference (the real audio oracle) — DESIGN

Per owner: don't diff two separate emulators (never sample-align). Embed a
reference DSP fed **identical** register writes + BRR + cycle ticks and diff
per-sample. The seed already exists: **`dsp_shadow`** runs a parallel dry-mix
(currently a "better cubic interpolation" enhancement, opt-in
`SNESRECOMP_AUDIO_SHADOW`, default off, byte-identical when off) with a
`ShadowVerifier` that already judges shadow-vs-canon every sample. Plan:
- Promote the shadow from "better-interp enhancement" to a **faithful reference
  S-DSP** (blargg/bsnes math, embedded) fed the same inputs; emit per-sample
  divergence (max/RMS, per-stage) into the `audio_trace` rings.
- Immediate win available now with the EXISTING shadow: the canon-vs-cubic-shadow
  delta already quantifies the **gaussian-interpolation contribution to tone** —
  one of the prime off-tone suspects — with zero new code, just a divergence
  readout. (Audio DSP doubling is cheap; this is dev-mode, default off.)

**Open audio work (priority order):**
1. Add an always-on **shadow-divergence readout** to `audio_trace` (canon vs the
   existing cubic shadow) → first internal-reference tone number, no new deps.
2. Promote `dsp_shadow` to a full faithful reference S-DSP for per-stage attribution.
3. Self-A/B the **immediate-KON vs `#if !MY_CHANGES` hardware-cycle KON** path.
4. ~~Off-cue cure ties to Axis 2 (real SPC clock from a real cycle estimate).~~
   **TRIED + DISPROVEN 2026-06-28 — see "Off-cue experiment" below.**
5. Robust pitch tracker (the dominant-bin metric is unreliable for polyphony).
6. Extend to MMX / Zelda themes and SFX-heavy scenes.

### Off-cue experiment (2026-06-28) — master-clock SPC pacing REGRESSES, reverted

**Hypothesis (Axis-2→Axis-5 tie):** replace the synthetic `+256-main-cycles-per-
APU-touch` SPC pacing (`cpu_pace_cycles` → `rtl_accumulate_apu_catchup`, ×2/7)
with a *real* clock — pace the SPC from the recompiler's region-weighted MASTER-
clock accumulator. **Built it fully:** added `cpu->master_cycles` (companion to
`cpu->cycles`), emitted per-block `CPU cycles × code-region speed` (memsel-aware;
`recompiler/snes_cycles.py` `region_speed`), and drove `apuCatchupCycles` from
the per-touch master delta (`× 1.024 MHz / 21.477 MHz`; the all-fast case reduces
to the old 2/7). 22/22 cycle tests pass; SMW regen+build clean; booted fine (no
watchdog stall — the fast recomp spin keeps the handshake over-clocking).

**Measured A/B vs the bsnes oracle (SMW attract, full-overlap, identical window):**

| build | pacing | drift | onset match | \|err\| med |
|---|---|---|---|---|
| BEFORE | +256/touch | −4013 ppm | 53% (24/45) | 8.0 ms |
| AFTER  | master-clock | **−5900 ppm** | **29% (18/63)** | **28.0 ms** |
| REVERT | +256/touch | −4013 ppm | 53% (24/45) | 8.0 ms |

**Verdict: REGRESSION.** Master-clock pacing made the SPC run *faster* (drift more
negative), halving onset match. The REVERT capture reproduces BEFORE bit-for-bit
— which also proves **SMW attract is deterministic** and the A/B metric is
reliable (so the regression is real, not capture noise).

**Root cause:** `master_cycles` counts every recompiled block executed per
*wall-frame*, but the recomp runs frames **host-driven** (60 fps via the host
loop), NOT by counting 357368 master clocks/frame. Its per-frame execution-cycle
total therefore does NOT track real elapsed time (spin-wait / non-HLE'd polling
inflate it), so multiplying by region speed *over-paces* the SPC. The crude
`+256/touch` is decoupled from this and happens to sit closer. **Pacing the SPC
from accumulated GUEST-EXECUTION cycles is structurally wrong** — the SPC is a
real-time clock domain; it must be paced by WALL time / consumer rate, which the
audio thread (`RtlRenderAudio`, consumer-rate top-up) already does.

**Reverted:** `rtl_accumulate_apu_catchup` restored to `+256/touch`. The
`master_cycles` accumulator + `g_memsel` tracking are KEPT as **inert** Axis-5
infra (mirrors the already-inert `cpu->cycles`; tested; near-free) so a *correct*
pacing attempt can reuse it — they drive nothing now.

### Off-cue follow-up (2026-06-28) — RING measurement + a measurement-bug fix

After the failed pacing experiment, **measured** the steady-state SPC production
via the always-on `audio_trace` ring (`audio_stats`, no pause), instead of
guessing again. Two decisive results:

1. **The SPC is paced 98.3% by the AUDIO THREAD (consumer rate), 1.7% by the
   CPU-thread catch-up.** (SMW attract, 33 s window: produced 1,131,546 = 19,267
   CPU + 1,112,279 audio; 0 drops.) So the `cpu_pace_cycles`/`rtl_accumulate_apu_
   catchup` path — the one the failed experiment rewired — drives almost none of
   the tempo. **Any off-cue fix must change the audio-thread / `dsp_getSamples`
   path, not the CPU-thread catch-up.** This is the deeper reason master-clock
   pacing couldn't help.
2. **The native S-DSP production rate is 32040.3 samples/s — i.e. exactly the
   intended 32040 Hz** (`apuCyclesPerMaster = 32040*32/(1364*262*60)`; byuu's
   measured real-SNES rate; bsnes uses it too). The steady-state rate is *correct*.

**Measurement bug found + fixed:** `audio_trace_dump_wav` hardcoded a **32000 Hz**
WAV header for the 32040-rate PCM ring. So every `audio_ab_diff` run resampled the
recomp 32000→32040 against the 32040 oracle — a systematic ~1250 ppm stretch +
onset misalignment that **inflated the apparent off-cue**. Fixed the header to
32040 (`runner/src/audio_trace.c`; dev-only dump path, no player-audio change).

A/B effect (SMW attract vs bsnes), recomp WAV labeled correctly:

| label | drift | onset | timbre LSD |
|---|---|---|---|
| 32000 (bug) | −4013 ppm (consistent) | 53% | 4.3 dB |
| 32040 (fixed) | +754 .. −2272 ppm (noisy, weak corr) | 51–71% | **2.7 dB** |

The systematic −4000 ppm is gone; the residual drift is small and **noise-
dominated** (varies ±2000 ppm by capture window at corr ~0.2 — this metric is not
trustworthy for <~kppm claims). The robust, repeatable gain is **LSD 4.3 → 2.7 dB**
(spectral match; less alignment-sensitive). **Conclusion: the recomp's steady-state
audio tempo is substantially correct; the "off-cue" was largely a dump-rate
mislabel plus an unreliable drift metric.**

**Next off-cue hypotheses (un-tried), now better-targeted:**
- A trustworthy **per-sample tempo oracle** is needed (the `audio_ab_diff` drift
  is too noisy at corr ~0.2). The internal lockstep S-DSP reference (dsp_shadow,
  below) is the right instrument — diff per-sample note-event timing, not a
  cross-correlation lag slope on two separately-produced WAVs.
- If a real residual remains, audit **command scheduling** (`RtlApuWrite` anchors
  each port write in SPC-sample time from wall gaps) for hardware-faithful target
  placement — that shifts *when* notes trigger, the true "cue".
- The CPU-thread catch-up (1.7%) is NOT the lever; don't re-touch it.

## Axis 5 (cont.) — PPU / video · **VERIFIED PIXEL-EXACT vs bsnes (2026-06-28)**

- Scanline rasterizer (bsnes/LakeSnes lineage, `ppu.c`), all modes 0-7 + windows
  + mosaic + sprites. Frame produced in one burst by the game-side draw loop;
  **no free-running dot clock** (`inVblank` never set; comment `ppu.c:847`).
- DMA/HDMA functionally modeled (8 modes, indirect HDMA) but **timing-transparent**
  — `dmaTimer` is not fed back to the scheduler. Always-on `$420B` DMA trace ring.

### Framebuffer diff — recomp output is BIT-IDENTICAL to bsnes (2026-06-28)

Built a two-process per-frame **framebuffer** diff (more complete than VRAM-only:
tests guest→VRAM/OAM/CGRAM *and* the rasterizer end-to-end; frames are discrete +
deterministic so frame-N alignment is clean, unlike audio). Infra:
- recomp: debug-server `dump_frame_raw <N> <path>` — arms a NON-PAUSING capture in
  `debug_server_record_frame` (re-renders the present PPU into a private 256x224
  buffer, the proven `cmd_screenshot` path; RULE-0 safe) and writes raw BGRX.
- oracle: `tools/snesref` `cb_video` dumps listed frames raw (env
  `SNESREF_FRAME_DUMP_DIR/_FROM/_TO/_STEP`); converts 0RGB1555 (bsnes 115's format)
  → BGRX to match.
- analysis: `tools/ppu_frame_diff.py` — per-frame exact/tolerant pixel match, MAD,
  PSNR, boot-offset search.

**Result (SMW title, authentic 4:3):** recomp frame 300 == bsnes 504 and recomp
700 == bsnes 904 — a single constant boot offset **+204** (bsnes runs the full real
boot; the recomp HLEs it ~204 frames faster) — each at **100.00% exact pixel match,
ZERO of 57,344 pixels different**, sprites included. The recomp's PPU output is
**bit-identical to the bsnes oracle**. (Gotcha that masked it first pass: the dev
config had `Widescreen=1` + `NoSpriteLimits=1` → the 256-wide crop grabbed the
16:9 left-extension and matched ~36%; set authentic 4:3 → 100%.)

*Caveat:* verified on the title screen (BG + animated sprites + palette). The
attract DEMO (scrolling/HDMA/sprite-heavy gameplay) is not yet bit-aligned — those
frames were sprite-phase-offset in the coarse search; a fine-aligned demo pass is
the obvious extension. Title-screen bit-exactness is already strong evidence the
rasterizer + DMA-to-VRAM path are faithful.

## Axis 6 — Static-vs-dynamic recompiler fidelity · **STRONG**

- Dispatch completeness (no missed indirect/jump-table targets), Tier-2 interp
  floor, gap manifest — the most-developed axis. Differential tool: `wram_diff.py`
  + the snesref WRAM-lo per-frame trace (the psx `divergence/` analog).
- **Note:** the multi-tier interp (`interp816.c`) lived on the
  `feat/multi-tier-interp-fallback` / `investigate` lineage, NOT on `main`; the
  audio stack (`audio_trace`) + launcher + shadow/msu1 live on `main`. No single
  branch was the union.
- **RECONCILE DONE (2026-06-27): `reconcile/cycle-multitier`.** Merged
  `integ/sm-interp` (multi-tier) into the cycle-accuracy branch — 4 conflicts,
  all combined (audio + multi-tier trace files, tailcall/abandon infra). The
  union now has: main's audio/launcher/shadow/msu1 + the Axis-2 cycle work +
  the multi-tier interp/bridge + dispatch tier-down. v2 suite 241/246 (only the
  5 pre-existing stale RTS-ABI tests fail). **SMW builds + runs:** repointed the
  junction, full regen (multi-tier tier-down cleared the unresolved-IndirectGoto
  stubs → `interp_tier_dispatch_balanced`), synced funcs.h, inited the
  RmlUi/freetype submodules + built their libs, added `ppu_dma_trace.c` to the
  vcxproj (SMW branch `reconcile/multitier-cycle-build`). **Production|x64
  built, 0 errors; soak = PASS:** ~130 s of attract (frames 59→7859) at a
  locked 60 fps (one 59 at first-second ramp), zero watchdog/abandon/stub-fire/
  crash. The cycle emit runs in a real built game at full speed. Slowdown
  measured via an env-gated FPS heartbeat (`SNESRECOMP_FPS`, dev-only).

## Axis 7 — Determinism · **assumed, untracked**

- The WRAM-diff and audio-diff loops presuppose run-to-run reproducibility from a
  fixed start. No dedicated tracking; the always-on frame fingerprint (psx
  Layer-1 analog) is not yet ported.

---

## Reusable SNES-portable kit (status)

| psx methodology | SNES status |
|---|---|
| Axis taxonomy + living burndown | **this doc** |
| Reference shelf + GREEN gate | partial (bsnes source on hand; no test ROMs wired) |
| Two-process ring-buffer diff | **audio: done**; MMIO/VRAM: rings exist on recomp, need oracle side |
| Two-anchor REGION cycle gate | **mechanism done** (`tools/cyc_watch`, ring + REGION, validated vs interp816); real-ROM ground truth still needs the bsnes cycle hook |
| Three-layer first-divergence | WRAM-lo layer exists; fingerprint/read-watch not ported |
| Coverage audits, fail-closed | opcode decode fail-closed; codegen/dispatch audits exist |
| State-surface diffs | audio sample-stream **done**; VRAM/event-ring next |

## Artifacts in this worktree
- `tools/snesref/frontend.cpp` — bsnes oracle WAV capture (robust header)
- `tools/audio_ab_diff.py` — drift-tolerant A/B analyzer
- `_audio_ab/` — `smw_oracle_bsnes.wav`, `smw_recomp_boot.wav`, `smw_ab.json`
