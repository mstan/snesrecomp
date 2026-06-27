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
| **Cycle timing** | *(none yet)* — would require a bsnes source hook | bsnes is open-source and cycle-stepped; a `bsnes_total_guest_cycles()` export read from snesref mirrors the psx Beetle `beetle_total_guest_cycles` pattern. Not built. |
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
- **Gaps:** no CPU **test-ROM** validation (the psx Amidog analog). The real
  semantic risk is abstract (M,X)-width soundness, tracked in
  `docs/ABSTRACT_INTERPRETATION_GAPS.md`.
- **Next lever:** run a 65816 test ROM (e.g. a SNES CPU test) on recomp vs bsnes,
  diff result registers — highest-leverage single validator, not yet built.

## Axis 2 — Cycle / timing · **COST MODEL LANDED (step A); reference/emit pending**

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
  **Remaining for B:** wrap interp816 in a SNES bus (ROM map + WRAM + synthetic
  MMIO) to free-run a real ROM to an anchor — OR go straight to the bsnes hook
  as the real-ROM ground truth (the accurate full-system oracle is better
  suited to real-ROM cycle truth than a CPU-only interpreter). Owner sign-off
  pending on the bsnes build commitment.
- **C. Recomp emit:** the recompiler emits exact accumulated charges collapsing
  to a **per-block integer constant** (near-free; stays fast). Delay-of-control
  and branch/page-cross owned by the block bundle (psx P2 lesson: don't lose a
  cycle at block-leader/branch boundaries). **Segmented charge** at every
  guest-visible time observation (MMIO to timers/PPU/DMA/IRQ) so both backends
  observe devices at the same architectural boundary.
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
4. Off-cue cure ties to Axis 2 (real SPC clock from a real cycle estimate).
5. Robust pitch tracker (the dominant-bin metric is unreliable for polyphony).
6. Extend to MMX / Zelda themes and SFX-heavy scenes.

## Axis 5 (cont.) — PPU / video · **SCANLINE-accurate render, frame-accurate timing**

- Scanline rasterizer (bsnes/LakeSnes lineage, `ppu.c`), all modes 0-7 + windows
  + mosaic + sprites. Frame produced in one burst by the game-side draw loop;
  **no free-running dot clock** (`inVblank` never set; comment `ppu.c:847`).
- DMA/HDMA functionally modeled (8 modes, indirect HDMA) but **timing-transparent**
  — `dmaTimer` is not fed back to the scheduler. Always-on `$420B` DMA trace ring.
- **Next lever:** per-frame VRAM byte-diff vs bsnes (the psx `axis5_gpu` VRAM-diff
  analog); requires exposing VRAM from the oracle (snesref currently exposes only
  WRAM-lo via `RETRO_MEMORY_SYSTEM_RAM`).

## Axis 6 — Static-vs-dynamic recompiler fidelity · **STRONG**

- Dispatch completeness (no missed indirect/jump-table targets), Tier-2 interp
  floor, gap manifest — the most-developed axis. Differential tool: `wram_diff.py`
  + the snesref WRAM-lo per-frame trace (the psx `divergence/` analog).
- **Note:** the multi-tier interp (`interp816.c`) lives on the
  `feat/multi-tier-interp-fallback` / `investigate` lineage, NOT on `main`; the
  audio stack (`audio_trace`) + launcher + shadow/msu1 live on `main`. **No single
  branch is the union** — a reconcile is owed before a clean game build tracks
  both. (This worktree builds against `main`; the SMW vcxproj's `interp816`
  reference is uncommitted local multi-tier work.)

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
