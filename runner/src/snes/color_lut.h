// color_lut.h — present-time screen-color simulation for SNES (C).
//
// PRESENT-TIME ONLY. Never touches emulation or the verify path: the raw
// renderBuffer (0x00RRGGBB, the frame-hashed / oracle output) is left
// untouched; this maps a COPY for display. Default "raw" = exact passthrough,
// so default output is byte-identical. Opt-in via SNESRECOMP_SCREEN.
//
// The model is first-principles CIE colorimetry (standard SMPTE-C / NTSC CRT
// phosphors → sRGB, CRT gamma) — published standards, not guessed per-console
// SNES-CRT measurements. Caveat: applied to the brightness-scaled 8-bit
// framebuffer (5-bit recovered via >>3), matching the GBA approach; a future
// hook at the CGRAM 15-bit level would be more precise.
//
// Color-science core ported from gbarecomp src/runtime/color_lut.cpp, itself
// from JRickey/gba-recomp crates/screen, © Jrickey, MIT OR Apache-2.0.

#ifndef SNESRECOMP_COLOR_LUT_H
#define SNESRECOMP_COLOR_LUT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Build the LUT from SNESRECOMP_SCREEN (raw|crt|trinitron). Call once at
// startup (re-callable). Returns 1 if a non-passthrough model is active.
int snes_color_lut_setup(void);
int snes_color_lut_active(void);

// Map `n` 0x00RRGGBB pixels from src into dst (graded). dst is for PRESENT
// only. If no model is active this is never called (caller presents src raw).
void snes_color_lut_map(const uint32_t* src, uint32_t* dst, size_t n);

#ifdef __cplusplus
}
#endif

#endif  // SNESRECOMP_COLOR_LUT_H
