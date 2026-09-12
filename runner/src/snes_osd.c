/*
 * snes_osd.c — host OSD: FPS readout, turbo indicator, timed toasts.
 *
 * Ported from psxrecomp runtime/src/host_osd.c. The font table below and the
 * toast expiry/dirty-flag shape are that file's; the FPS averaging and the
 * status-line composition are new, because psxrecomp's host composes its own
 * status string and the SNES side deliberately does not (see snes_osd.h).
 *
 * Two things this file will not do, both learned from the PSX original:
 *
 *  - It never touches guest memory, VRAM, or a savestate. The overlay is
 *    composited after the PPU is done, so a peer showing a toast and a peer
 *    not showing one still agree about the emulated machine.
 *  - It never presents by itself. Deciding when pixels reach the screen is
 *    the host's job; this file hands over an image and is told when it landed.
 */

#include "snes_osd.h"

#include <stdio.h>
#include <string.h>

#include "desktop/sdl_compat.h"

#define OSD_MAX_CHARS  64
#define OSD_PAD_X      2
#define OSD_PAD_Y      2
#define OSD_SCALE      2
#define OSD_DEFAULT_MS 2000
#define OSD_GLYPH_W    8
#define OSD_GLYPH_H    8

/* Rolling FPS window. 64 frames is ~1s at 60Hz — long enough to stop the
 * readout flickering on a single slow frame, short enough to react. */
#define OSD_FPS_HISTORY 64

/* Public-domain 8x8 ASCII (32..126), row bitmasks LSB = left pixel.
 * Sourced from the font8x8_basic set (https://github.com/d7samurai/font8x8). */
static const uint8_t FONT8X8[95][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /*   */
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, /* ! */
    {0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00}, /* " */
    {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00}, /* # */
    {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00}, /* $ */
    {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00}, /* % */
    {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00}, /* & */
    {0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00}, /* ' */
    {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00}, /* ( */
    {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00}, /* ) */
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, /* * */
    {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00}, /* + */
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06}, /* , */
    {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00}, /* - */
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00}, /* . */
    {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00}, /* / */
    {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00}, /* 0 */
    {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00}, /* 1 */
    {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00}, /* 2 */
    {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00}, /* 3 */
    {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00}, /* 4 */
    {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00}, /* 5 */
    {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00}, /* 6 */
    {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00}, /* 7 */
    {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00}, /* 8 */
    {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00}, /* 9 */
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00}, /* : */
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06}, /* ; */
    {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00}, /* < */
    {0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00}, /* = */
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, /* > */
    {0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00}, /* ? */
    {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00}, /* @ */
    {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00}, /* A */
    {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00}, /* B */
    {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00}, /* C */
    {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00}, /* D */
    {0x7F,0x06,0x06,0x3E,0x06,0x06,0x7F,0x00}, /* E */
    {0x7F,0x06,0x06,0x3E,0x06,0x06,0x06,0x00}, /* F */
    {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00}, /* G */
    {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00}, /* H */
    {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* I */
    {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00}, /* J */
    {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00}, /* K */
    {0x06,0x06,0x06,0x06,0x06,0x06,0x7F,0x00}, /* L */
    {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00}, /* M */
    {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00}, /* N */
    {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00}, /* O */
    {0x3F,0x66,0x66,0x3E,0x06,0x06,0x06,0x00}, /* P */
    {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00}, /* Q */
    {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00}, /* R */
    {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00}, /* S */
    {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* T */
    {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00}, /* U */
    {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00}, /* V */
    {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}, /* W */
    {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00}, /* X */
    {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00}, /* Y */
    {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00}, /* Z */
    {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00}, /* [ */
    {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00}, /* \ */
    {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00}, /* ] */
    {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00}, /* ^ */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, /* _ */
    {0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00}, /* ` */
    {0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00}, /* a */
    {0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00}, /* b */
    {0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00}, /* c */
    {0x38,0x30,0x30,0x3e,0x33,0x33,0x6E,0x00}, /* d */
    {0x00,0x00,0x1E,0x33,0x3f,0x03,0x1E,0x00}, /* e */
    {0x1C,0x36,0x06,0x0f,0x06,0x06,0x0f,0x00}, /* f */
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F}, /* g */
    {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00}, /* h */
    {0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00}, /* i */
    {0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E}, /* j */
    {0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00}, /* k */
    {0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* l */
    {0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00}, /* m */
    {0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00}, /* n */
    {0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00}, /* o */
    {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F}, /* p */
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78}, /* q */
    {0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00}, /* r */
    {0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00}, /* s */
    {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00}, /* t */
    {0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00}, /* u */
    {0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00}, /* v */
    {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00}, /* w */
    {0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00}, /* x */
    {0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F}, /* y */
    {0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00}, /* z */
    {0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00}, /* { */
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, /* | */
    {0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00}, /* } */
    {0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00}, /* ~ */
};

/* ---- state -------------------------------------------------------------- */

static char     s_msg[OSD_MAX_CHARS];
static Uint32   s_expire_ms;
static int      s_msg_active;

static int      s_fps_visible;
static int      s_turbo;

static float    s_fps_history[OSD_FPS_HISTORY];
static float    s_fps_average;
static int      s_fps_pos;
static int      s_fps_filled;
static Uint64   s_fps_last_tick;

/* Composed status line ("60 FPS", "60 FPS  TURBO"), rebuilt only when one of
 * its inputs actually changes — a per-frame snprintf into the toast path
 * would re-rasterize the image every frame for a number that moves rarely. */
static char     s_status[OSD_MAX_CHARS];
static int      s_status_active;

static int      s_img_dirty = 1;
static int      s_needs_clear;

#define OSD_IMG_W  ((OSD_PAD_X * 2 + OSD_MAX_CHARS * OSD_GLYPH_W) * OSD_SCALE)
#define OSD_IMG_H  ((OSD_PAD_Y * 2 + OSD_GLYPH_H * 2 + 1) * OSD_SCALE)
static uint32_t s_img[OSD_IMG_W * OSD_IMG_H];
static int      s_img_w;
static int      s_img_h;

static SDL_Texture  *s_tex;
static int           s_tw, s_th;
static SDL_Renderer *s_ren;

/* ---- helpers ------------------------------------------------------------ */

static int msg_visible(void) {
    if (!s_msg_active) return 0;
    if ((int32_t)(SDL_GetTicks() - s_expire_ms) >= 0) {
        s_msg_active = 0;
        s_msg[0] = '\0';
        s_needs_clear = 1;
        s_img_dirty = 1;
        return 0;
    }
    return 1;
}

/* Defined below; the status line must use the WINDOW-CORRECTED average, not
 * the raw accumulator. The raw one is diluted by the still-zeroed history
 * slots for the first 64 frames, which is why the readout used to climb from
 * 0 to 60 over its first second instead of simply being right. */
float snes_osd_fps(void);

/* Rebuild the status line from fps + turbo. Marks the image dirty only when
 * the text actually changed, so a steady 60 fps rasterizes once. */
static void status_refresh(void) {
    char next[OSD_MAX_CHARS];
    if (!s_fps_visible && !s_turbo) {
        next[0] = '\0';
    } else if (s_fps_visible && s_turbo) {
        snprintf(next, sizeof(next), "%d FPS  TURBO", (int)(snes_osd_fps() + 0.5f));
    } else if (s_fps_visible) {
        snprintf(next, sizeof(next), "%d FPS", (int)(snes_osd_fps() + 0.5f));
    } else {
        snprintf(next, sizeof(next), "TURBO");
    }
    if (strcmp(next, s_status) == 0) return;
    memcpy(s_status, next, sizeof(next) < sizeof(s_status) ? sizeof(next) : sizeof(s_status));
    s_status[sizeof(s_status) - 1] = '\0';
    const int was_active = s_status_active;
    s_status_active = (s_status[0] != '\0');
    if (was_active && !s_status_active) s_needs_clear = 1;
    s_img_dirty = 1;
}

/* ---- FPS ---------------------------------------------------------------- */

void snes_osd_toggle_fps(void) { snes_osd_set_fps_visible(!s_fps_visible); }

int snes_osd_fps_visible(void) { return s_fps_visible; }

void snes_osd_set_fps_visible(int on) {
    on = on ? 1 : 0;
    if (on == s_fps_visible) return;
    s_fps_visible = on;
    if (on) {
        /* Start from a clean window. Showing an average built partly from
         * before the readout was opened reports a number for frames the
         * player never saw. */
        s_fps_pos = 0;
        s_fps_filled = 0;
        s_fps_average = 0.0f;
        s_fps_last_tick = 0;
        memset(s_fps_history, 0, sizeof(s_fps_history));
    }
    status_refresh();
}

void snes_osd_note_frame(void) {
    const Uint64 now = SDL_GetPerformanceCounter();
    const Uint64 freq = SDL_GetPerformanceFrequency();
    if (s_fps_last_tick && freq) {
        const Uint64 delta = now - s_fps_last_tick;
        if (delta > 0) {
            const float inst = (float)((double)freq / (double)delta);
            /* Rolling mean over the window. Subtracting the slot being
             * overwritten keeps this O(1) rather than re-summing 64 floats
             * every frame. */
            s_fps_average += (inst - s_fps_history[s_fps_pos]) /
                             (float)OSD_FPS_HISTORY;
            s_fps_history[s_fps_pos] = inst;
            s_fps_pos = (s_fps_pos + 1) % OSD_FPS_HISTORY;
            if (s_fps_pos == 0) s_fps_filled = 1;
            if (s_fps_visible) status_refresh();
        }
    }
    s_fps_last_tick = now;
}

float snes_osd_fps(void) {
    /* Before the window has filled once the mean is diluted by the zeroed
     * slots, so scale it up rather than under-report during the first second. */
    if (!s_fps_filled) {
        if (s_fps_pos == 0) return 0.0f;
        return s_fps_average * (float)OSD_FPS_HISTORY / (float)s_fps_pos;
    }
    return s_fps_average;
}

/* ---- turbo -------------------------------------------------------------- */

void snes_osd_set_turbo(int on) {
    on = on ? 1 : 0;
    if (on == s_turbo) return;
    s_turbo = on;
    status_refresh();
}

int snes_osd_turbo(void) { return s_turbo; }

/* ---- toasts ------------------------------------------------------------- */

void snes_osd_push(const char *msg, int duration_ms) {
    if (!msg || !msg[0]) return;
    if (duration_ms <= 0) duration_ms = OSD_DEFAULT_MS;
    snprintf(s_msg, sizeof(s_msg), "%s", msg);
    s_expire_ms = SDL_GetTicks() + (Uint32)duration_ms;
    s_msg_active = 1;
    s_needs_clear = 0;
    s_img_dirty = 1;
}

void snes_osd_push_slot_saved(int slot) {
    char buf[OSD_MAX_CHARS];
    snprintf(buf, sizeof(buf), "Slot %d saved", slot);
    snes_osd_push(buf, 0);
}

void snes_osd_push_slot_loaded(int slot) {
    char buf[OSD_MAX_CHARS];
    snprintf(buf, sizeof(buf), "Slot %d loaded", slot);
    snes_osd_push(buf, 0);
}

void snes_osd_push_slot_empty(int slot) {
    char buf[OSD_MAX_CHARS];
    snprintf(buf, sizeof(buf), "Slot %d empty", slot);
    snes_osd_push(buf, 0);
}

/* ---- rasterizing --------------------------------------------------------- */

/* Glyphs into any ARGB buffer, at OSD_SCALE. (x, y) are unscaled origins. */
static void draw_text_into(uint32_t *buf, int bw, int bh, int x_cells, int y_px,
                           const char *msg, uint32_t colour) {
    int n = (int)strlen(msg);
    if (n > OSD_MAX_CHARS) n = OSD_MAX_CHARS;
    for (int ci = 0; ci < n; ci++) {
        unsigned char ch = (unsigned char)msg[ci];
        if (ch < 32 || ch > 126) ch = '?';
        const uint8_t *g = FONT8X8[ch - 32];
        for (int row = 0; row < OSD_GLYPH_H; row++) {
            const uint8_t bits = g[row];
            for (int col = 0; col < OSD_GLYPH_W; col++) {
                if (!(bits & (1u << col))) continue;
                const int x0 = (x_cells + ci * OSD_GLYPH_W + col) * OSD_SCALE;
                const int y0 = (y_px + row) * OSD_SCALE;
                for (int dy = 0; dy < OSD_SCALE; dy++)
                    for (int dx = 0; dx < OSD_SCALE; dx++) {
                        const int x = x0 + dx, y = y0 + dy;
                        if ((unsigned)x < (unsigned)bw && (unsigned)y < (unsigned)bh)
                            buf[y * bw + x] = colour;
                    }
            }
        }
    }
}

static void draw_text_row(const char *msg, int row_y) {
    draw_text_into(s_img, s_img_w, s_img_h, OSD_PAD_X, row_y, msg, 0xFFFFFFFFu);
}

/* ---- volume bar ---------------------------------------------------------- */

#define VOL_SHOW_MS   1500
#define VOL_BAR_W     8      /* unscaled: the meter itself */
#define VOL_BAR_H     64
#define VOL_PANEL_W   (OSD_PAD_X * 2 + 3 * OSD_GLYPH_W + 2)   /* "100%" fits */
#define VOL_PANEL_H   (OSD_PAD_Y * 2 + VOL_BAR_H + 3 + OSD_GLYPH_H + 1)
#define VOL_IMG_W     (VOL_PANEL_W * OSD_SCALE)
#define VOL_IMG_H     (VOL_PANEL_H * OSD_SCALE)
static uint32_t s_vol_img[VOL_IMG_W * VOL_IMG_H];
static int      s_vol_percent = -1;
static uint32_t s_vol_expire_ms;
static int      s_vol_active, s_vol_dirty;

static void fill_rect_into(uint32_t *buf, int bw, int bh, int x, int y, int w, int h,
                           uint32_t colour) {
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            if ((unsigned)xx < (unsigned)bw && (unsigned)yy < (unsigned)bh)
                buf[yy * bw + xx] = colour;
}

static void volume_rasterize(void) {
    const int S = OSD_SCALE;
    memset(s_vol_img, 0, sizeof(s_vol_img));
    /* The same translucent grey panel as the status line. */
    fill_rect_into(s_vol_img, VOL_IMG_W, VOL_IMG_H, 0, 0, VOL_IMG_W, VOL_IMG_H, 0xC0202020u);
    const int bar_x = (VOL_PANEL_W - VOL_BAR_W) / 2 * S;
    const int bar_y = OSD_PAD_Y * S;
    /* Trough, then the level from the bottom, then tick marks every 25%. */
    fill_rect_into(s_vol_img, VOL_IMG_W, VOL_IMG_H, bar_x, bar_y, VOL_BAR_W * S, VOL_BAR_H * S, 0xFF505050u);
    const int level = (VOL_BAR_H * s_vol_percent + 50) / 100;
    fill_rect_into(s_vol_img, VOL_IMG_W, VOL_IMG_H, bar_x, bar_y + (VOL_BAR_H - level) * S,
                   VOL_BAR_W * S, level * S, s_vol_percent ? 0xFFFFFFFFu : 0xFF808080u);
    for (int q = 1; q < 4; q++) {
        const int ty = bar_y + (VOL_BAR_H * q / 4) * S;
        fill_rect_into(s_vol_img, VOL_IMG_W, VOL_IMG_H, bar_x - S, ty, S, S, 0xFFC0C0C0u);
        fill_rect_into(s_vol_img, VOL_IMG_W, VOL_IMG_H, bar_x + VOL_BAR_W * S, ty, S, S, 0xFFC0C0C0u);
    }
    char pct[8];
    snprintf(pct, sizeof(pct), "%d%%", s_vol_percent);
    const int cells = (int)strlen(pct);
    const int text_x = (VOL_PANEL_W - cells * OSD_GLYPH_W) / 2;
    draw_text_into(s_vol_img, VOL_IMG_W, VOL_IMG_H, text_x, OSD_PAD_Y + VOL_BAR_H + 3, pct, 0xFFFFFFFFu);
    s_vol_dirty = 0;
}

void snes_osd_note_volume(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    if (percent != s_vol_percent) s_vol_dirty = 1;
    s_vol_percent = percent;
    s_vol_active = 1;
    s_vol_expire_ms = SDL_GetTicks() + VOL_SHOW_MS;
}

static int volume_visible(void) {
    if (!s_vol_active) return 0;
    if ((int32_t)(SDL_GetTicks() - s_vol_expire_ms) >= 0) {
        s_vol_active = 0;
        s_needs_clear = 1;
        return 0;
    }
    return 1;
}

int snes_osd_volume_image(const uint32_t **pixels, int *w, int *h) {
    if (!volume_visible()) {
        if (pixels) *pixels = NULL;
        if (w) *w = 0;
        if (h) *h = 0;
        return 0;
    }
    if (s_vol_dirty) volume_rasterize();
    if (pixels) *pixels = s_vol_img;
    if (w) *w = VOL_IMG_W;
    if (h) *h = VOL_IMG_H;
    return 1;
}

/*
 * The status line and a toast can be visible at once (turbo held while a slot
 * is saved), so the image is up to two rows tall and sized to the longer of
 * the two. psxrecomp's original is single-row because its host owns the status
 * string and never shows both.
 */
static void rasterize(void) {
    const int show_msg = msg_visible();
    const int show_status = s_status_active;
    const size_t msg_len = show_msg ? strlen(s_msg) : 0;
    const size_t status_len = show_status ? strlen(s_status) : 0;
    size_t widest = msg_len > status_len ? msg_len : status_len;
    if (widest > OSD_MAX_CHARS) widest = OSD_MAX_CHARS;

    const int rows = (show_msg ? 1 : 0) + (show_status ? 1 : 0);
    s_img_w = (int)((OSD_PAD_X * 2 + widest * OSD_GLYPH_W) * OSD_SCALE);
    s_img_h = (OSD_PAD_Y * 2 + rows * OSD_GLYPH_H + (rows > 1 ? 1 : 0)) * OSD_SCALE;
    if (s_img_w < 1) s_img_w = 1;
    if (s_img_h < 1) s_img_h = 1;
    if (s_img_w > OSD_IMG_W) s_img_w = OSD_IMG_W;
    if (s_img_h > OSD_IMG_H) s_img_h = OSD_IMG_H;

    for (int i = 0; i < s_img_w * s_img_h; i++)
        s_img[i] = 0xFF202020u;

    int y = OSD_PAD_Y;
    if (show_status) { draw_text_row(s_status, y); y += OSD_GLYPH_H + 1; }
    if (show_msg)    { draw_text_row(s_msg, y); }
    s_img_dirty = 0;
}

/* ---- presenting ---------------------------------------------------------- */

int snes_osd_needs_present(void) {
    if (msg_visible() || s_status_active || volume_visible()) return 1;
    return s_needs_clear;
}

int snes_osd_image(const uint32_t **pixels, int *w, int *h) {
    const int show_msg = msg_visible();
    if (!show_msg && !s_status_active) {
        if (pixels) *pixels = NULL;
        if (w) *w = 0;
        if (h) *h = 0;
        return 0;
    }
    if (s_img_dirty) rasterize();
    if (pixels) *pixels = s_img;
    if (w) *w = s_img_w;
    if (h) *h = s_img_h;
    return 1;
}

void snes_osd_present_done(void) { s_needs_clear = 0; }

void snes_osd_draw_sdl(struct SDL_Renderer *renderer_in) {
    SDL_Renderer *renderer = (SDL_Renderer *)renderer_in;
    if (!renderer) return;

    const uint32_t *px = NULL;
    int w = 0, h = 0;
    if (snes_osd_image(&px, &w, &h) && px && w > 0 && h > 0) {
        /* Recreate on size change AND on renderer change: a texture belongs to
         * the renderer that created it, and a rematch hands the host a new
         * one. Reusing a texture across renderers is a use-after-free that
         * only shows up on the second session. */
        if (!s_tex || s_tw != w || s_th != h || renderer != s_ren) {
            if (s_tex) SDL_DestroyTexture(s_tex);
            s_tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                      SDL_TEXTUREACCESS_STREAMING, w, h);
            s_tw = w;
            s_th = h;
            s_ren = renderer;
            if (s_tex) SDL_SetTextureBlendMode(s_tex, SDL_BLENDMODE_BLEND);
        }
        if (s_tex) {
            SDL_UpdateTexture(s_tex, NULL, px, w * 4);
            /* Window-space, not the game rect: the readout is host chrome and
             * should not stretch with the aspect-corrected frame or slide
             * around when widescreen changes the frame's width. */
            const SDL_Rect dst = { 8, 8, w, h };
            snesrecomp_sdl_render_texture(renderer, s_tex, NULL, &dst);
        }
    }
    snes_osd_present_done();
}
