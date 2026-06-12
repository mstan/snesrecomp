# Shadow Audio + Screen Enhancements (SNES backport)

Backport of the gbarecomp "verified-enhancement" QoL layer to snesrecomp.
All work lives on the `feat/shadow-enhancements` branch / `_shadow_snesrecomp`
worktree — it does not touch the in-flight `fix/apu-audio-accuracy`,
`feat/*-widescreen`, etc.

## Governing principle (the carve-out)

Faithfulness is the product; these are an opt-in layer on top. The one
permitted form of HLE here is a **verified-enhancement shadow**, allowed only
when ALL hold:

1. The emulated (canon) path keeps running and stays both the authoritative
   output and the verify oracle. The shadow is never ground truth.
2. The shadow is continuously, differentially checked against the canon stream
   and substitutes only after a proven window.
3. It reverts loudly (logs DEGRADED) the instant it stops matching.
4. It is opt-in and present-time, off by default; with it off the output is
   byte-identical (frame hashes / verify / sweeps stay on the raw canon).

Worst-case failure is "the user hears/sees the authentic hardware output," and
it cannot mask a recompiler bug because the canon path it shadows is still the
thing being diffed. (Same rule now in `recomp-template/PRINCIPLES.md`,
"Verified-Enhancement HLE Is Allowed; Load-Bearing HLE Is Not".)

## What ports verbatim vs what is SNES-specific

| Piece | Status | Notes |
|---|---|---|
| **`ShadowVerifier`** (envelope-correlation self-check, auto-gain, prove/strike/pause) | **DONE** — `runner/src/snes/audio_shadow.{c,h}`, re-implemented in C, compiles clean | Engine-agnostic; identical algorithm to gbarecomp |
| Color-science core (xyY→XYZ, primaries→matrix, Bradford, sRGB OETF) | TODO | Lifts verbatim; reused for the screen LUT |
| Present-path color LUT | **DONE (runner module)** — `runner/src/snes/color_lut.{c,h}`, in `runner.cmake`, compiles clean | CIE color core (C) + CRT models (SMPTE-C `crt`, `trinitron`) replacing GBA's LCD panels. Present-time `0x00RRGGBB`→graded map (5-bit recovered via >>3, like GBA), `SNESRECOMP_SCREEN={raw,crt,trinitron}`, default `raw`=passthrough (identity). Raw renderBuffer (frame-hashed oracle) untouched. **Present hook is game-side** (the SDL present lives in each game's `main.c`, outside this worktree) — see wiring note below |
| **S-DSP shadow render** | **DONE** — `runner/src/snes/dsp_shadow.{c,h}`, wired into `dsp_cycle`, in `runner.cmake`, compiles clean (`-Wall -Wextra`) | Cubic (Catmull-Rom) re-render of the BRR voices in float vs the hardware 4-tap Gaussian; substitutes the dry mix only when the verifier proves it, reverts loud. Opt-in `SNESRECOMP_AUDIO_SHADOW` (default off → byte-identical). Echo path unchanged (applies to the chosen dry). Verifier auto-gain absorbs scale, so only structure must match; a mis-phased render simply falls back to canon + logs DEGRADED (signal to refine the interpolation phase) |

## Integration points (found on `main`)

- **Canon audio:** `runner/src/snes/dsp.c` — `dsp_cycle()` mixes all channels →
  `totalL/totalR` → `dsp->sampleBuffer` ring (stereo, power-of-two). Per-voice
  render is `dsp_cycleChannel()` (BRR decode + Gaussian interp + ADSR →
  `ch->sampleOut`). Echo in `dsp_handleEcho()`. The host consumes native
  samples from the ring (~`dsp.c:567`). → Feed (canon, shadow) to the verifier
  per output sample; substitute the shadow into the ring only when `proven`.
- **Canon video:** `runner/src/snes/ppu.c` — `renderBuffer` (xbgr pixel
  buffer, 256×224/240). → Apply the color LUT present-time (raw stays the
  oracle).
- **Build:** per-game `CMakeLists.txt` (e.g. `SuperMarioWorldRecomp/`) pulls in
  `runner/src`. New `runner/src/snes/*.c` files must be added there (or are
  globbed — verify). Test a game build (SMW) once wiring lands.
- **Gating:** env + (eventually) per-game config, default OFF, mirroring
  gbarecomp's `GBARECOMP_AUDIO_SHADOW` / `[audio] shadow` and
  `GBARECOMP_SCREEN` / `[video] screen`.

## Next steps

1. ~~S-DSP shadow render~~ — **DONE** (see table; `dsp_shadow.{c,h}` wired).
2. ~~Color LUT~~ — **DONE** as a runner module (`color_lut.{c,h}`). Game-side
   present wiring (each game's `main.c`, outside this worktree): at startup
   call `snes_color_lut_setup()`; if `snes_color_lut_active()`, before the
   SDL present run the finished `renderBuffer` through `snes_color_lut_map(raw,
   present_copy, w*h)` and upload the COPY (never modify the raw renderBuffer
   that gets frame-hashed). Identical shape to the GBC/Genesis present hooks.
3. Build a game (SMW) against this worktree; confirm default-off is
   byte-identical (audio + video), then A/B the enhancement. Audio compiles
   standalone; a full game build is the integration gate.
4. If the audio shadow logs DEGRADED in practice, refine the cubic phase to
   match the canon Gaussian's segment exactly (the verifier will then engage).

## Attribution

`ShadowVerifier` ported from JRickey/gba-recomp (`crates/gba-core/src/shadow.rs`)
via the gbarecomp C++ port, © Jrickey, MIT OR Apache-2.0, used with permission.
