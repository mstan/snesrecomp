# SNES performance burn-down

This worktree exists to create enough uncapped headroom for lower-power hosts,
including the original Xbox, without weakening the faithful SNES hardware
model. Performance is measured as uncapped frames per second, not as the
ability to sit at a paced 60 FPS.

The original Xbox is a budget target for CPU, memory, and cache pressure. The
measurements below were taken on a modern Windows desktop and are not hardware
certification for an Xbox port.

The approach is adapted from the useful NDS work:

- measure complete, repeatable workloads rather than boot time;
- attribute time before changing architecture;
- compile diagnostic hot-path work out of production;
- move invariant work from pixels to tiles or spans;
- preserve a faithful fallback when adding a specialized fast path;
- use interleaved, order-balanced A/B runs and retain rejected experiments;
- require state, framebuffer, audio, attract-mode, gameplay, and fuzz gates.

## Current baseline

Super Mario World and Mega Man X already provide
`--benchmark <frames> <rom>`. It disables pacing, VSync, audio, autosave, and
the launcher and emits a machine-readable `SNESRECOMP_BENCHMARK` record. These
two games are the initial control workloads. Before accepting broad changes,
the benchmark surface should be shared with more titles.

The first audit also found several costs that are not part of emulating SNES
hardware:

- `RtlRunFrame` hashes all 128 KiB of WRAM every frame for the always-on
  determinism ring.
- `ppudma_frame_snapshot` scans 256 CGRAM entries and 32,768 VRAM words every
  frame, in addition to retaining the PPU and DMA rings.
- the indirect-dispatch path writes an always-on 1,024-entry diagnostic ring;
- the audio path records every native DSP output sample into a 16 MiB PCM
  history and records DSP writes and host-consume events into an 8 MiB event
  history.

These are high-confidence first measurements, not automatic deletions. Some
aggregate counters may be needed for audio pacing or reports even when the
large histories are disabled. Split functional state and cheap production
health counters from forensic history before measuring the production-off
path.

The PPU is about 97 KiB because it implements multiple hardware modes, color
math, windows, mosaic, large tiles, sprites, overlays, and the shared
widescreen capability. Its size is not by itself evidence of waste. Mode 1's
ordinary 2bpp and 4bpp paths already operate a tile at a time. Several less
common paths remain scalar per pixel, including 8bpp, large tiles, offset-
per-tile modes, mosaic, and Mode 7. Optimize the measured modes in place; do
not collapse specialized paths into a single abstraction unless performance
and exactness both survive.

## Measurement contract

1. Use Release builds from the same compiler and source revision.
2. Build at BelowNormal priority and with one build job on this machine.
3. Warm both binaries, then run at least five order-balanced A/B pairs.
4. Report paired deltas and raw medians. Reject contaminated samples.
5. Use a fixed frame interval that reaches stable attract or gameplay, not
   only logos and boot.
6. Record native and enabled-widescreen results separately.
7. Add coarse host phase brackets for guest frame, post-frame PPU draw, audio
   render/consume, host presentation, and non-hardware diagnostics. Do not
   treat these brackets as exclusive CPU/PPU/APU subsystem timings. Timing
   must be optional or compiled out for the final A/B.
8. Pin framebuffer/state hashes and dispatch/interpreter miss counts for every
   compared run. Audio-enabled correctness runs are separate from the
   audio-disabled throughput benchmark.

A candidate is retained when the order-balanced median is positive outside
run noise and the full correctness gates pass. Code-size changes are reported
because instruction-cache pressure matters on the Xbox.

## Retained results

The September 2026 title measurements below use each title's default
`SNESRECOMP_EXECUTION_MODE=lle` path. In SMW and MMX that means a faithful LLE
scheduler/main-loop is interpreted, while guest calls bounce into generated AOT
bodies when a `g_dispatch_table` entry exists; missing bodies keep using the
interpreter fallback. These results are therefore default production
rich-LLE/AOT-bounce measurements, not pure generated-AOT-only runs and not pure
whole-program interpreter runs.

### Final retained aggregate, 2026-09-05

The final retained source tree uses the accepted diagnostic/DSP caching work,
default SMALL audio history, restored original PPU source, and no CPU
bus-guard candidate. It was rebuilt from the coherent 20260905b title build
dirs with `SNESRECOMP_ENABLE_BENCHMARK_PHASES=OFF`; the current build cleared
any cached audio-history override and exercised the CMake default
(`SNESRECOMP_AUDIO_TRACE_HISTORY=1`). Framework baseline `94c1197` used the
same title source, compiler, ROMs, renderer, and staged config policy.
These are campaign baseline-to-retained-source measurements. The integration
base (`origin/main` at `c2f2421`) already contains independent runtime/title
work, so these percentages are not an additional speedup over that current main
branch.

Native-width, audio-disabled throughput:

- Super Mario World, 15,000 frames, five order-balanced pairs: baseline94
  median 626.296 FPS, retained current median 1,180.543 FPS, paired median
  **+88.496%**, range `+81.575%..+92.369%`.
- Mega Man X, 24,000 frames, five order-balanced pairs: baseline94 median
  968.194 FPS, retained current median 1,633.738 FPS, paired median
  **+69.411%**, range `+62.108%..+70.462%`.

Enabled-widescreen confirmation:

- Super Mario World, 15,000 frames, two order-balanced pairs: baseline94
  median 635.969 FPS, retained current median 1,195.050 FPS, paired median
  **+87.910%**, range `+87.622%..+88.198%`.
- Mega Man X, 24,000 frames, two order-balanced pairs: baseline94 median
  870.840 FPS, retained current median 1,306.958 FPS, paired median
  **+50.081%**, range `+49.539%..+50.622%`.

All timing records had fresh `last_run_report.json` snapshots, Direct3D11 with
VSync off, and one matching frame/main-cycle status per title
(`15000/601344512` for SMW, `24000/423197184` for MMX). The widescreen runs
created wider windows (`1194x672` SMW, `1284x720` MMX), so they exercised the
enhancement path. Independent final correctness gates matched original94 to
the final retained tree for BMP, WRAM, and APURAM in native and widescreen
modes for both titles.

Static section deltas, retained current minus baseline94:

- Super Mario World: executable bytes `-1,387`, `.text -2,112`, `.data +32`,
  `.bss -27,476,000`.
- Mega Man X: executable bytes `-1,899`, `.text -2,080`, `.data +32`,
  `.bss -27,475,968`.

The static BSS reduction is larger than the isolated SMALL audio-history delta
because it also includes the accepted production diagnostic-history removals.
For MMX, the unrelated desktop UI `s_icons` BSS remains present and is not
counted as SNES core state. These final measurements supersede historical
original94 aggregates that included the later-rejected PPU helper candidate.

Evidence:

- `build/perf/final-20260905-retained/artifact_manifest.json`
- `build/perf/final-20260905-retained/measurement_summary.json`
- `build/perf/final-20260905-retained/timings/smw_baseline94_vs_current_small_native_5p.json`
- `build/perf/final-20260905-retained/timings/mmx_baseline94_vs_current_small_native_5p.json`
- `build/perf/final-20260905-retained/timings/smw_baseline94_vs_current_small_wide_2p.json`
- `build/perf/final-20260905-retained/timings/mmx_baseline94_vs_current_small_wide_2p.json`

### Stabilized September 2026 runtime gates

A September 2026 aggregate run initially showed a severe regression in the
then-current `build-codex-perf-current-full` binaries. That regression is
excluded from all retained gains: it came from a newly introduced lazy
`SNESRECOMP_WLOG_ADDR` gate that returned before caching the disabled state,
leaving every CPU write to call `getenv`. The bad binaries and one stale-object
cached-bridge relink are retained only as rejected evidence.

After rebuilding from the fixed base, caching the interpreter bridge's
`SNESRECOMP_APU_PORT_DIAG` check and testing the address predicate before the
diagnostic gate produced the first retained September result. The diagnostic is
still presence-based: setting the variable, including to `0`, enables it. A
fresh-process C gate covers unset, empty, and `0` environments on APU-port and
non-port accesses, including byte writes, word reads, and word writes.

Controls used Release MinGW title builds, native-width output, audio disabled,
direct3d11 with VSync off, and order-balanced pairs from the stabilized
WLOG-fixed binaries. Fresh `last_run_report.json` snapshots were captured for
every run, and each matrix had one unique status/WRAM snapshot.

- Super Mario World, 3,000 frames: WLOG-fixed FULL to cached-bridge FULL had
  paired median **+61.747%**; raw medians were 619.889 FPS and 999.391 FPS.
- Mega Man X, 6,000 frames: WLOG-fixed FULL to cached-bridge FULL had paired
  median **+31.217%**; raw medians were 1,105.594 FPS and 1,422.610 FPS.

The cached-bridge candidate also compared positively against the clean
`94c1197` framework baseline under the same title/source/compiler controls:
SMW improved from 648.896 to 1,048.228 FPS (**+71.305%**) and MMX improved
from 1,066.928 to 1,456.819 FPS (**+35.739%**). These aggregate comparisons
do not count the introduced WLOG regression as a win; they use the rebuilt
fixed candidate. However, the historical original94-vs-cached-bridge binaries
included the later-rejected PPU composition helper candidate, so they are not
the final aggregate baseline comparison. Use the final retained aggregate
above for headline throughput and size deltas. Code-size delta versus the
WLOG-fixed base was small: about +256 bytes text in the linked executable,
+16 bytes data in the `interp_bridge` object, and no BSS change.

Evidence:

- `build/perf/cachedbridge/smw_fixedfull_vs_cachedbridge_5pairs_wlogrsp.json`
- `build/perf/cachedbridge/mmx_fixedfull_vs_cachedbridge_5pairs_wlogrsp.json`
- `build/perf/cachedbridge/smw_original94_vs_cachedbridge_5pairs.json`
- `build/perf/cachedbridge/mmx_original94_vs_cachedbridge_5pairs.json`

### DSP inactive-gate caching

With the cached bridge retained, the DSP/APU lane rebuilt a candidate that
keeps inactive diagnostic checks out of hot DSP paths while preserving the
explicit diagnostic and state/cross-state behavior. The matrix used the same
native-width, audio-disabled, direct3d11/VSync-off benchmark contract and
fresh report validation as above.

- Super Mario World, 3,000 frames: cached-bridge FULL to DSP-gated FULL had
  paired median **+6.605%**; raw medians were 1,115.780 FPS and 1,201.521 FPS.
- Mega Man X, 6,000 frames: cached-bridge FULL to DSP-gated FULL had paired
  median **+10.496%**; raw medians were 1,790.293 FPS and 1,975.162 FPS.

Evidence:

- `build/perf/dspgate/20260905-032354/smw_cachedbridge_vs_dspgate_5pairs.json`
- `build/perf/dspgate/20260905-032354/mmx_cachedbridge_vs_dspgate_5pairs.json`

### Production frame fingerprints

The first retained change compiles the full-WRAM fingerprint hash and its
8,192-entry ring out of trace-off production builds. Trace builds enable the
history by default, co-simulation forces it on, and an explicit
`SNESRECOMP_ENABLE_FRAME_FINGERPRINTS=ON` remains available independently.

Controls used MinGW GCC 15.2, SDL 3.4.12, Release `-O3`, native-width output,
audio disabled, and 3,000 fully simulated/rendered frames. Framework baseline
code was `868df63`; title revisions were SMW `98edb9e` and MMX `5324bf5`.
Five order-balanced pairs produced:

- Super Mario World: paired deltas `+2.719%, +19.139%, +18.447%, +12.624%,
  +6.677%`; paired median **+12.624%**. Raw medians were 278.870 FPS baseline
  and 322.397 FPS candidate.
- Mega Man X: paired deltas `+7.502%, +8.486%, +5.407%, +5.256%, +7.340%`;
  paired median **+7.340%**. Raw medians were 397.349 FPS baseline and
  421.703 FPS candidate.

Both titles produced byte-identical 128 KiB WRAM dumps at frame 2,999:
SMW SHA-256 `e395adef1c70fc59ab916f32c6d4e1ca9e4b420e16a5b2390b0bc661b45c8299`
(`crc32_wram=9c4e595c`) and MMX SHA-256
`31b29ca41c1108c97e4c491ad8bd5db65ede97924b9426530a3bda9dca5e632c`
(`crc32_wram=e0f6a7e2`). GNU `size` reports 65,536 fewer BSS bytes and 128
fewer text bytes in each candidate. A fresh trace configuration selected
fingerprints ON and successfully compiled both `common_rtl.c` and the real
`debug_server.c` at BelowNormal priority with one job.

### Production PPU/DMA forensic history

Trace-off production builds now omit the 4,096-frame PPU snapshot ring, the
8,192-event DMA ring, and the per-frame scan that counts non-zero values in
all 256 CGRAM entries and 32,768 VRAM words. The public recording API remains
present as no-op stubs so callers do not need configuration-dependent source.
Trace builds enable the full implementation by default, co-simulation forces
it on, and `SNESRECOMP_ENABLE_PPU_DMA_HISTORY=ON` enables it independently.

This smaller cost was near the noise floor of the current desktop harness.
Five order-balanced pairs produced:

- Super Mario World, 3,000 frames: paired deltas `+0.047%, +5.364%, +21.420%,
  -0.015%, +1.945%`; paired median **+1.945%**. Raw medians were 326.297 FPS
  baseline and 326.249 FPS candidate.
- Mega Man X, 6,000 frames: paired deltas `+8.765%, -7.804%, +1.554%,
  -5.071%, +5.336%`; paired median **+1.554%**. Raw medians were 505.134 FPS
  baseline and 494.419 FPS candidate.

The paired medians are positive, but the spread and contradictory raw medians
mean these measurements establish throughput neutrality rather than a precise
speedup. The production footprint result is unambiguous: GNU `size` reports
294,976 fewer BSS bytes, about 4 KiB less text, and 32 fewer data bytes in
each title. Both titles again produced byte-identical WRAM at frame 2,999
using the hashes above. A trace-on build selected the real history
implementation and compiled `ppu_dma_trace.c` successfully at BelowNormal
priority with one job.

### Production indirect-dispatch history

Production now retains the whole-run exact-AOT-hit and interpreter-miss
counters while omitting the 1,024-entry detailed indirect-dispatch ring.
Trace builds retain the ring by default, co-simulation forces it on, and
`SNESRECOMP_ENABLE_DISPATCH_HISTORY=ON` remains available independently.
Five order-balanced 3,000-frame pairs produced:

- Super Mario World: paired deltas `-24.894%, -0.130%, +10.993%, +1.595%,
  -4.095%`; paired median **-0.130%**. Raw medians were 319.768 FPS baseline
  and 319.353 FPS candidate, establishing that this cost is immaterial there.
- Mega Man X: paired deltas `-1.252%, +9.132%, +45.777%, +8.226%, +5.428%`;
  paired median **+8.226%**. Raw medians were 354.883 FPS baseline and
  374.147 FPS candidate. The third pair was visibly host-contaminated, but
  removing it still leaves three of four pairs positive and a positive median.

GNU `size` reports 24,576 fewer BSS bytes and about 1.7 KiB less text per
title. Both candidates produced byte-identical WRAM at frame 2,999 using the
same hashes as the preceding gates. A trace-on build selected and compiled
the real ring implementation.

## Rejected experiments

### Audio PCM/event/snapshot history culling

An experiment preserved functional pacing, sample-clock, occupancy, drop,
underflow, consume-quantum, and port-overwrite counters while compiling out
the 16 MiB PCM ring, 8 MiB event ring, snapshot ring, and per-sample snapshot
clock check. It was rejected before commit: five order-balanced 3,000-frame
SMW pairs had deltas `-5.073%, -6.266%, -6.497%, +13.572%, -1.379%` (median
**-5.073%**), and the first two MMX pairs were **-16.607%** and **-29.740%**.
That satisfies the two-negative-experiment stop condition. The large static
footprint saving is not sufficient reason to accept a repeatable throughput
regression; audio history remains unchanged pending attribution of why its
removal affects runtime layout or pacing.

Follow-up attribution now has explicit build-time variants instead of another
one-way cull:

- `SNESRECOMP_AUDIO_TRACE_HISTORY=FULL`: historical PCM/event/snapshot rings.
- `SNESRECOMP_AUDIO_TRACE_HISTORY=SMALL`: reduced rings for targeted captures.
- `SNESRECOMP_AUDIO_TRACE_HISTORY=COUNTERS`: production counters only.
- `SNESRECOMP_AUDIO_TRACE_HISTORY=RESERVED`: full storage reserved but
  hot-path history writes disabled, isolating layout/footprint effects from
  write bandwidth.

CMake now defaults to `SMALL` after the stabilized title A/B and paced/audio
validation below. Trace and co-simulation builds force `FULL`. A ROM-free C
regression gate verifies that produced/consumed/drop/underflow/pacing/port
counters retain the same contract in all four modes. On MinGW GCC, the probe
measured `sizeof(AudioTraceEvent)=16` and `sizeof(AudioTraceSnap)=40`; full
history reserves 16 MiB PCM, 8 MiB events, and 160 KiB snapshots before other
`audio_trace` globals. `size audio_trace.o` reported:

- `COUNTERS`: text 3,536, data 16, BSS 448.
- `SMALL`: text 4,792, data 16, BSS 545,184.
- `FULL`: text 4,792, data 16, BSS 25,330,080.
- `RESERVED`: text 3,616, data 16, BSS 25,330,112.

Those are static/object-size facts only. Because the normal `--benchmark`
path disables audio, accepting an audio-history default change still requires
audio-enabled smoke/correctness runs plus order-balanced title timing after
other build-heavy agents are idle.

The first DSP-gated title attribution did not accept a throughput default
change. COUNTERS saves the 25,329,664 bytes of core forensic BSS listed above
in the linked title binaries, but five-pair throughput was inside A/A noise:

- Super Mario World, 3,000 frames: FULL to COUNTERS paired median **+0.104%**,
  range `-6.955%..+0.815%`.
- Mega Man X, 6,000 frames: FULL to COUNTERS paired median **-0.314%**, range
  `-3.988%..+3.319%`.

A/A controls were noisier than this effect (`-0.446%..+6.068%` for SMW and
`-9.101%..-6.578%` for MMX), so this is a memory-policy candidate, not an
accepted performance win. RESERVED 2-pair attribution was also noisy
(SMW `+0.871%`, MMX `+7.921%`) and is insufficient for a decision. Mega Man X
also contains an unrelated desktop UI `s_icons` BSS allocation of about
16 MiB; do not count that as SNES core state or as part of the audio-history
budget.

Evidence:

- `build/perf/audio_variants/20260905-dspgate/measurement_summary.json`
- `build/perf/audio_variants/20260905-dspgate/audio_variant_sizes_hashes.json`

The later stabilized SMALL probe used fresh coherent title builds after the
WLOG/APU/DSP diagnostic fixes, restored PPU source, and dropped CPU bus-guard
candidate. It relinked the fresh Ninja edge with only `audio_trace.c` rebuilt
for `FULL` or `SMALL`. The `audio_trace.c.obj` code size was identical
(`.text 4,672`) while SMALL reduced core forensic BSS from 25,330,080 bytes to
545,184 bytes, saving 24,784,896 bytes (about 23.64 MiB). Linked title BSS
changed from 26,188,656 to 1,403,760 bytes for SMW, and from 42,971,472 to
18,186,576 bytes for MMX. The MMX number still includes its unrelated desktop
UI `s_icons` buffer in both variants; do not count that as SNES core state.

Five order-balanced native LLE title pairs with audio disabled were positive
or neutral:

- Super Mario World, 15,000 frames: FULL to SMALL paired median **+1.294%**,
  range `+0.079%..+1.641%`.
- Mega Man X, 24,000 frames: FULL to SMALL paired median **+0.141%**, range
  `-0.999%..+1.615%`.

Every record used fresh reports, Direct3D11 with vsync off, and identical
per-title frame/main-cycle status (`15000/601344512` for SMW and
`24000/423197184` for MMX). SMALL then passed native and enabled-widescreen
paced correctness on SMW and MMX using the same fresh restored-PPU/no-CPUguard
small-probe artifacts. This accepts SMALL as the audio-history memory policy;
the runtime default-promotion build/test step passed, and CMake now defaults to
SMALL for normal production builds. The earlier COUNTERS/RESERVED results
remain opt-in/provisional evidence only.

Final correctness artifact:
`build/perf/final_correctness_20260905c/summary.json`, SHA-256
`A04AFF2B64C54E2C7140F9E3585638447AB43BDB7F29EDC031C4060D6C655954`.
The manifest is `build/perf/final_correctness_20260905c/manifest.json`,
SHA-256 `5F9AD9FDD64075958496AD353FB4B007DC0A04061186167F248165D2E7A48D1E`.
The eight paced audio-only cases all reached an accepted attempt with
`audio_health.ok:true`, `dropped_audible:0`, `enqueue_failures:0`, and nonzero
host/DSP peak. Two first-attempt audio-health failures were preserved rather
than relaxed: SMW FULL wide failed once with `dropped_audible=382`, then passed
retry; MMX SMALL wide failed once with `dropped_audible=291`, then passed
retry.

Accepted-attempt private bytes showed the expected footprint reduction:
SMW FULL native/wide `89.6/87.9 MB` versus SMALL `64.0/62.8 MB`; MMX FULL
native/wide `107.2/108.3 MB` versus SMALL `81.2/83.5 MB`. Sampled working-set
values were SMW FULL native/wide `74.6/75.8 MB` versus SMALL `66.5/57.8 MB`,
and MMX FULL native/wide `73.9/81.0 MB` versus SMALL `68.4/69.9 MB`.
Working set is host-residency evidence only; it need not equal the static BSS
delta because the 50-second DSP capture does not touch the full historical PCM
ring.

Separate BMP/WRAM/APURAM captures matched FULL vs SMALL for SMW native, SMW
wide, MMX native, and MMX wide. Each case captured 11 BMPs. SMW WRAM/APURAM
hashes were `40e580b34efc0e04fe3a596812430768d109e75bd89e7d2e6d47dc8523c40a8e`
and `47b94efce71ad05c27ac4f94b5499c0aaeb4bb82af77c42e33af659ff89d6fd3`;
MMX WRAM/APURAM hashes were
`1cb2ffeadda8f60bf71992961e78c04a556021ed761f4ac800a7ff6b5cde7064` and
`29ac403e02d0fd2fb02e193e243956d9346acc1f3776fb353ff8e304c2b31acc`.

Evidence:

- `build/perf/20260905b-smallprobe/{smw,mmx}/{full,small}/manifest.json`
- `build/perf/20260905b-smallprobe/timings/smw_full_vs_small_5p_native.json`
- `build/perf/20260905b-smallprobe/timings/mmx_full_vs_small_5p_native.json`
- `build/perf/final_correctness_20260905c/summary.json`

Final integration checks after the default promotion:

- PhaseON SMW/MMX desktop smoke passed with `phase_timing:true`,
  `phase_semantics:"inclusive"`, positive `guest_frame`, `ppu_draw`, and
  `host_present` calls, no benchmark flags on generated objects, and
  `SNESRECOMP_AUDIO_TRACE_HISTORY=1` on `audio_trace.c` from the SMALL default.
  Artifact: `build/perf/final_phaseon_20260905/summary.json`, SHA-256
  `3B10B742845C0110EF09AF7253EDF7F5F2647FE7E3340E656427566F9C253C85`.
- Original94-vs-final retained state/video passed for MMX and SMW, native and
  enabled-widescreen: 11 BMP hashes, WRAM trace, and APURAM trace matched for
  each title/mode. Artifact:
  `build/perf/final_original94_state_20260905d/summary.json`, SHA-256
  `FB0E46BDE0A4A41207910672AFEC3F3CA08A70E66DDB690B84CB71AB96355F1B`.
- Fresh SMALL-default SMK headless 3,000-frame scripted-input route passed
  strict qualification. Fresh SMALL-default SMRPG headless 3,000-frame
  save/load route produced valid JSON but did not qualify due to 598 audio
  underruns. Controls showed the same 598-underrun save/load failure on an
  origin-main positional binary and the current FULL helper build; final SMALL
  without save/load reduced this to one underrun but still exited 8. This is a
  pre-existing SMRPG headless qualification issue tracked by `beads-23p`, not a
  SMALL regression. Artifact:
  `build/perf/final_headless_20260905/final_summary.json`, SHA-256
  `3A45E2CAED781F82C335DA4F1EC888ED9223AB68C00589FEF151D839C56B2B36`.

## Burn-down

### P0 — harness and attribution

- [ ] Put the finite uncapped benchmark loop behind a shared runner API and
  wire it into public title worktrees rather than duplicating title logic.
- [ ] Add optional phase timing and production diagnostic counters to the
  benchmark JSON.
- [ ] Establish native-width SMW and MMX controls, then add one workload for
  each materially different hardware route: offset-per-tile, Mode 7/DSP-1,
  Super FX, Cx4, and SA-1.
- [ ] Store the exact commands, compiler, title/framework revisions, frame
  interval, ROM identity, and raw A/B records.

### P1 — production observability culling

- [x] Measure disabling the full-WRAM frame fingerprint while preserving an
  explicit diagnostic/cosim option.
- [x] Split the PPU/DMA forensic rings from cheap production counters. In
  particular, do not scan all VRAM and CGRAM every frame when forensic capture
  is absent.
- [x] Split the audio PCM/event histories from the counters required for
  pacing, underrun reporting, and user-visible diagnostics. The production
  default is now `SMALL`; trace/co-sim still force `FULL`.
- [x] Compile the dispatch-event history out of production while retaining
  hit/miss aggregates if they measure cheaply and remain useful.
- [x] Report executable and static/BSS size as well as throughput.

Each item is a separate commit and A/B. This phase should precede renderer or
CPU changes so later profiles describe emulation rather than diagnostics.

### P2 — PPU hot paths

- [ ] Attribute PPU time by mode and subpath before editing it.
- [ ] Apply the NES result where it actually fits: fetch tilemap entries and
  planar row data once per tile/span, then emit pixels. First candidates are
  scalar 8bpp and 16x16 large-tile paths.
- [ ] In offset-per-tile modes, keep offset lookup at authentic segment
  boundaries but hoist the selected tile and planar row out of the inner pixel
  loop.
- [ ] Measure native composition separately from widescreen shadow/policy and
  overlay work. Skip inactive enhancement policy early without changing the
  enabled path.
- [ ] Only investigate Mode 7, mosaic, sprite evaluation, color math, or a
  decoded-tile cache when attribution makes one material.
- [ ] Keep mode-specific paths. A DRY refactor is accepted only when generated
  code size, speed, and all mode-specific framebuffer gates are no worse.

This is an optimization of the existing emulated PPU, not a title-specific
replacement. Enhancements remain optional clients of the shared PPU and must
not alter native guest-visible state.

### P3 — CPU, bus, and generated dispatch

- [ ] Profile direct WRAM/ROM accesses, hardware-register routing, generated
  calls, binary-search dispatch, interpreter fallback, watchdog hooks, and
  cycle charging separately.
- [ ] Try header-inline fast paths only for mappings whose semantics are
  completely identical, retaining the shared slow path for MMIO, mutable code,
  tracing, and unusual cartridge mappings.
- [ ] Measure direct or cached dispatch only where profiles show repeated
  lookup cost. Preserve live M/X variant selection, bank mirroring, RAM guards,
  non-local returns, and interpreter fallback.
- [ ] Audit generated code for production-only trace/shadow-stack work, but do
  not remove guest stack semantics or host-return bookkeeping needed for
  correct control flow.
- [ ] Track executable growth and reject wins likely to regress the Xbox
  instruction cache.

### P4 — APU, DSP, DMA, and coprocessors

- [ ] Benchmark with audio disabled for host throughput and with audio enabled
  for end-to-end budget and correctness.
- [ ] Attribute SPC700 execution, DSP synthesis, resampling/callback work,
  catch-up synchronization, and trace history independently.
- [ ] Profile DMA replay and avoid duplicate routing only if the two hardware
  models remain state-identical.
- [ ] Treat DSP-1, Super FX, Cx4, and SA-1 independently. Keep each LLE core as
  the faithful floor; add a host fast path or HLE only after a representative
  title proves the core is material and an exact differential gate exists.

### P5 — release and regression sweep

- [ ] Enumerate title repositories with a verified public remote at validation
  time. A 2026-09-05 read-only inventory of configured upstream remotes is
  recorded at `build/perf/title_inventory_top_level_20260905.json`; public
  accessibility verification and the full sweep remain open. A local title
  without a public repository is skippable.
- [ ] Build each public title in its own linked worktree, at BelowNormal
  priority and one job.
- [ ] Run its complete attract/demo loop where available, then a basic
  deterministic input fuzz that covers gameplay transitions. Use peripheral-
  specific fuzz for Super Scope/Mouse titles if public titles require it.
- [ ] Compare candidate and same-compiler baseline framebuffer hashes, WRAM
  fingerprints or equivalent state hashes, audio sample/hash gates, and
  dispatch/interpreter misses. Investigate every new miss.
- [ ] Include focused hardware coverage: ordinary Mode 1, windows/color math,
  offset-per-tile, Mode 7 plus DSP-1, Super FX, Cx4, SA-1, widescreen enabled,
  and native-width fallback.
- [ ] Run the framework unit, PPU, CPU differential, runtime-dispatch,
  coprocessor, and existing attract-demo regression suites.
- [ ] Document unavailable ROMs or other external blockers instead of silently
  treating them as passes.

## Rejected CPU Bus Candidate

The 2026-09-05 non-SA-1 CPU bus-latch guard was dropped. The candidate avoided
calling `cart_note_cpu_bus` for carts without SA-1, but the effect did not
clear the acceptance bar for a small-cache target. Five order-balanced native
title pairs using the fixed pre-guard CPU object as the only substitution
measured:

- Super Mario World, 15,000 frames: paired median **+1.062%**, range
  `+0.074%..+5.520%`.
- Mega Man X, 24,000 frames: paired median **+0.701%**, range
  `-0.684%..+4.905%`.

The same window's A/A controls were already about `-1.361%` for SMW and
`-0.971%` for MMX. The candidate also added about 192 bytes of object text
(`cpu_state.c.obj` `.text +0xc0`) and 512 bytes to the SMW executable. Keep
the SA-1 latch-ordering regression coverage, but do not carry this guard
forward or credit it as a retained optimization.

Evidence:

- `build/perf/20260905b-probes/timings/smw_cpu_guard_5p_native.json`
- `build/perf/20260905b-probes/timings/mmx_cpu_guard_5p_native.json`

## Rejected PPU Candidate

The 2026-09-05 native/inactive-policy composition helper refactor was dropped.
It added about 1 KiB of text in the PPU object, had a prior general-refactor
microbenchmark negative, and same-base title probes were mixed/noisy: SMW
varied up to about +6%, while MMX showed a negative median around -3.8% with
one run near -7% against A/A noise near 1%. Do not carry this helper extraction
forward as an optimization. Future PPU changes should start from attribution,
keep the original policy body intact unless measured otherwise, and compare
candidate output against HEAD with the composition regression fixture.

## Stop conditions

Stop a line of work when two well-formed experiments are neutral or negative,
when its measured bucket is too small to repay the complexity, or when it
requires weakening exactness. Keep the faithful implementation and the
experiment record. After the high-confidence diagnostic and tile/span work,
re-profile before deciding whether CPU dispatch, audio, or a coprocessor
deserves the next round.
