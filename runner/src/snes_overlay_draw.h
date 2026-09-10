#ifndef SNES_OVERLAY_DRAW_H
#define SNES_OVERLAY_DRAW_H

#include <stdint.h>

/*
 * Shared 2D primitives for the runner's in-game overlays.
 *
 * There were already TWO copies of an 8x8 font in this directory -- FONT8 in
 * snes_savestate_menu.c and FONT8X8 in snes_osd.c -- and snes_rewind.c carried
 * a comment claiming there was only one, which is why its filmstrip has a
 * progress bar instead of a caption: "rather than a second copy... keeping one
 * font in one file is worth a plainer caption". Adding text to the rewind
 * overlay would have made a third. This is that one file.
 *
 * Everything takes an explicit stride so a caller can draw into a panel of any
 * width; the savestate menu previously hardcoded SSM_W inside draw_char, which
 * is what made it unshareable.
 *
 * Colours are ARGB8888, matching what both overlays hand to the host.
 */

/* Runner input word bits, as produced by read_keyboard()/read_gamepad().
 * Restated here because every overlay needs them and each had its own copy;
 * snes_rewind.c's copy was WRONG (it used bit 5 for "Left" and bit 3 for "A"),
 * which is why its pad navigation responded to Up/Down and Start. */
#define SNES_PAD_B      (1u << 0)
#define SNES_PAD_Y      (1u << 1)
#define SNES_PAD_SELECT (1u << 2)
#define SNES_PAD_START  (1u << 3)
#define SNES_PAD_UP     (1u << 4)
#define SNES_PAD_DOWN   (1u << 5)
#define SNES_PAD_LEFT   (1u << 6)
#define SNES_PAD_RIGHT  (1u << 7)
#define SNES_PAD_A      (1u << 8)
#define SNES_PAD_X      (1u << 9)
#define SNES_PAD_L      (1u << 10)
#define SNES_PAD_R      (1u << 11)

/* Nav auto-repeat for overlays, in milliseconds: how long a held direction
 * waits before it starts repeating, and how fast it repeats after that.
 * Same numbers the save-state menu has always used, shared so the two
 * overlays feel identical under a held d-pad. */
#define SNES_OVL_REPEAT_DELAY 350u
#define SNES_OVL_REPEAT_RATE   90u

void snes_ovl_fill_rect(uint32_t *dst, int stride, int h_max,
                        int x0, int y0, int w, int h, uint32_t col);
void snes_ovl_stroke_rect(uint32_t *dst, int stride, int h_max,
                          int x, int y, int w, int h, uint32_t col);
void snes_ovl_fill_disc(uint32_t *dst, int stride, int h_max,
                        int cx, int cy, int r, uint32_t col);
void snes_ovl_draw_char(uint32_t *dst, int stride, int h_max,
                        int x0, int y0, char c, uint32_t col, int scale);
void snes_ovl_draw_text(uint32_t *dst, int stride, int h_max,
                        int x, int y, const char *s, uint32_t col, int scale);

/* A SNES face button: filled disc with its letter punched out of the middle.
 * The colours are the Super Famicom's, which is the closest thing the SNES has
 * to psxrecomp's shape-coded PlayStation glyphs. 18x18 including the ring. */
void snes_ovl_draw_button(uint32_t *dst, int stride, int h_max,
                          int x, int y, char label, uint32_t col);

/* The palette both overlays use for face buttons, so a hint row reads the same
 * wherever it appears. */
#define SNES_OVL_COL_A  0xFF6BE06Bu
#define SNES_OVL_COL_B  0xFFFF6B6Bu
#define SNES_OVL_COL_X  0xFF5FA8FFu
#define SNES_OVL_COL_Y  0xFFFFD24Du

#endif /* SNES_OVERLAY_DRAW_H */
