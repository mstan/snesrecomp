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
// Screen models aligned with mstan/psxrecomp revision
// d7815862e18ef939e5e6e5c6947f8c29667982d5 (PolyForm Noncommercial
// 1.0.0). Color-science lineage: JRickey/gba-recomp crates/screen,
// © Jrickey, MIT OR Apache-2.0. See third_party/psxrecomp_color_lut/.

#ifndef SNESRECOMP_COLOR_LUT_H
#define SNESRECOMP_COLOR_LUT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum SnesScreenKind {
  SNES_SCREEN_RAW = 0,
  SNES_SCREEN_CRT,
  SNES_SCREEN_COMPOSITE,
  SNES_SCREEN_TRINITRON,
  SNES_SCREEN_KIND_COUNT,
} SnesScreenKind;

// Build the LUT from SNESRECOMP_SCREEN
// (raw|crt|composite|trinitron). Call once at startup (re-callable). Returns
// 1 if a non-passthrough model is active, 0 for Raw, or -1 for invalid input
// or allocation failure.
int snes_color_lut_setup(void);
int snes_color_lut_setup_kind(int kind);
int snes_color_lut_kind_from_name(const char* name, int* kind);
const char* snes_color_lut_kind_name(int kind);
int snes_color_lut_active(void);

// Map `n` 0x00RRGGBB pixels from src into dst (graded). dst is for PRESENT
// only. If no model is active this is never called (caller presents src raw).
void snes_color_lut_map(const uint32_t* src, uint32_t* dst, size_t n);

#ifdef __cplusplus
}
#endif

#endif  // SNESRECOMP_COLOR_LUT_H
