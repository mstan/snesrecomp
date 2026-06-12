#ifndef SNESRECOMP_WIDESCREEN_H
#define SNESRECOMP_WIDESCREEN_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// Shared widescreen frontend asset (game-agnostic).
//
// The PPU-level *capability* lives in snes/ppu.c: PpuSetExtraSpace /
// PpuSetExtraSpaceCentered / PpuSetExtraSideSpace / PpuSetWidescreenHudSplit,
// all inert at the default (0 margin). This file owns the small pieces of the
// frontend that are byte-identical across every title that opts in, so they
// live in one place instead of being copy-pasted into each game's main.c:
//
//   * the runtime master switch (g_ws_active / g_ws_extra), which a game's
//     config sets once at startup and which the apply_overrides'd game-logic
//     snippets read via `extern bool g_ws_active;` — keeping the injected
//     code identical across games requires a single canonical symbol;
//   * the representational clamp constant (kWsExtraMax);
//   * the per-frame framebuffer present/copy loop.
//
// What stays per-game is *policy*: which screens get the wide view, how the
// visible margin is derived (SMW: a fixed symmetric border gated on the level
// game-mode; Zelda: a dynamic per-side margin clamped to live room/scroll
// state). Policy lives in each game's RtlDrawPpuFrame and calls the helpers
// below. See recomp-template/ENHANCEMENTS.md (Rule 1/2) for the discipline.
//
// Attribution: the extra-side-space PPU model is reimplemented from snesrev's
// zelda3 (https://github.com/snesrev/zelda3, MIT); see IMPROVEMENTS.md.

// True iff widescreen is active this run (i.e. g_ws_extra > 0). Read by the
// game-logic override snippets the build-time injector adds to generated code.
// With this false, every injected branch is not taken and behaviour is
// byte-identical to the faithful build. DEFINED BY EACH GAME (next to its
// config wiring), not by the runner — so a title that already defines it (SMW)
// adopts this header with no change. The injector's `extern bool g_ws_active;`
// resolves to that per-game symbol.
extern bool g_ws_active;

// Extra columns rendered per side, baked into the framebuffer width
// (the centering budget / capacity). 0 = authentic 256-wide output; the
// internal render width is 256 + 2*g_ws_extra. A game's config computes this
// once at startup; the per-frame visible margin (which may be smaller, e.g.
// clamped to room bounds) is set separately via PpuSetExtraSideSpace.
extern int g_ws_extra;

// Hard cap on g_ws_extra from the SNES 9-bit OAM x space (see ppu.c and
// ENHANCEMENTS Rule 5): the wrap threshold is 256+extra and the widest
// left-margin sprite tiles sit at 512-(64+extra), which must stay >= the
// threshold => 2*extra <= 192 => extra <= 95. Beyond this, outer-margin
// sprites are unrepresentable. Every consumer clamps to this same constant.
enum { kWsExtraMax = 95 };

// Per-frame present: copy the PPU's rendered framebuffer `src` (rows of
// row_bytes = snes_width*4, as written by the line renderer at that pitch)
// into the host surface `dst` at `pitch`. Identical across games — replaces
// the open-coded copy loop each main.c used to carry.
void RtlWidescreenPresent(uint8_t *dst, size_t pitch, const uint8_t *src,
                          int snes_width, int snes_height);

#endif  // SNESRECOMP_WIDESCREEN_H
