/* snes_overlay_draw.c — see snes_overlay_draw.h. */

#include <stddef.h>

#include "snes_overlay_draw.h"

/* ASCII 32..90 (space through 'Z'), row bitmasks LSB = left pixel. Lifted
 * verbatim from snes_savestate_menu.c, which no longer keeps its own. */
static const uint8_t FONT8[59][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00},
    {0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00}, {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00},
    {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00}, {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00},
    {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00}, {0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00},
    {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00}, {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00},
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06}, {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00}, {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00},
    {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00}, {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00},
    {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00}, {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00},
    {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00}, {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00},
    {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00}, {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00},
    {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00}, {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00},
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00}, {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06},
    {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00}, {0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00},
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, {0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00},
    {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00}, {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00},
    {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00}, {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00},
    {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00}, {0x7F,0x06,0x06,0x3E,0x06,0x06,0x7F,0x00},
    {0x7F,0x06,0x06,0x3E,0x06,0x06,0x06,0x00}, {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00},
    {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00}, {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00},
    {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00}, {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00},
    {0x06,0x06,0x06,0x06,0x06,0x06,0x7F,0x00}, {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00},
    {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00}, {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00},
    {0x3F,0x66,0x66,0x3E,0x06,0x06,0x06,0x00}, {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00},
    {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00}, {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00},
    {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00},
    {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00}, {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00},
    {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00}, {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00},
    {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00},
};

void snes_ovl_fill_rect(uint32_t *dst, int stride, int h_max,
                        int x0, int y0, int w, int h, uint32_t col)
{
    int x, y;
    for (y = y0; y < y0 + h; y++) {
        if (y < 0 || y >= h_max) continue;
        for (x = x0; x < x0 + w; x++) {
            if (x < 0 || x >= stride) continue;
            dst[y * stride + x] = col;
        }
    }
}

void snes_ovl_stroke_rect(uint32_t *dst, int stride, int h_max,
                          int x, int y, int w, int h, uint32_t col)
{
    snes_ovl_fill_rect(dst, stride, h_max, x, y, w, 1, col);
    snes_ovl_fill_rect(dst, stride, h_max, x, y + h - 1, w, 1, col);
    snes_ovl_fill_rect(dst, stride, h_max, x, y, 1, h, col);
    snes_ovl_fill_rect(dst, stride, h_max, x + w - 1, y, 1, h, col);
}

void snes_ovl_fill_disc(uint32_t *dst, int stride, int h_max,
                        int cx, int cy, int r, uint32_t col)
{
    int x, y;
    for (y = -r; y <= r; y++)
        for (x = -r; x <= r; x++)
            if (x * x + y * y <= r * r)
                snes_ovl_fill_rect(dst, stride, h_max, cx + x, cy + y, 1, 1, col);
}

void snes_ovl_draw_char(uint32_t *dst, int stride, int h_max,
                        int x0, int y0, char c, uint32_t col, int scale)
{
    int x, y, sx, sy;
    const uint8_t *g;
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    if (c < 32 || c > 90) c = '?';
    g = FONT8[(int)c - 32];
    for (y = 0; y < 8; y++) {
        uint8_t row = g[y];
        for (x = 0; x < 8; x++) {
            if ((row & (1u << x)) == 0) continue;
            for (sy = 0; sy < scale; sy++)
                for (sx = 0; sx < scale; sx++) {
                    int dx = x0 + x * scale + sx;
                    int dy = y0 + y * scale + sy;
                    if (dx >= 0 && dx < stride && dy >= 0 && dy < h_max)
                        dst[dy * stride + dx] = col;
                }
        }
    }
}

void snes_ovl_draw_text(uint32_t *dst, int stride, int h_max,
                        int x, int y, const char *s, uint32_t col, int scale)
{
    if (!s) return;
    while (*s) {
        snes_ovl_draw_char(dst, stride, h_max, x, y, *s++, col, scale);
        x += 8 * scale;
    }
}

void snes_ovl_draw_button(uint32_t *dst, int stride, int h_max,
                          int x, int y, char label, uint32_t col)
{
    snes_ovl_fill_disc(dst, stride, h_max, x + 9, y + 9, 9, col);
    snes_ovl_fill_disc(dst, stride, h_max, x + 9, y + 9, 8, 0xFF171B25u);
    snes_ovl_draw_char(dst, stride, h_max, x + 5, y + 5, label, col, 1);
}

/* ── Compositing an overlay panel into a host frame buffer ───────────────
 *
 * The overlays hand a host an ARGB panel and let it present that however it
 * presents anything. A host whose window is an SDL_Texture uploads it as a
 * second texture (the pattern in GundamWingEndlessDuelSNESRecomp's main.c).
 * A host with an ABSTRACTED presenter -- one that can be SDL_Renderer or
 * OpenGL depending on config, as SuperMetroidRecomp's is -- cannot do that
 * without writing the blit twice, once per backend.
 *
 * So: composite into the frame buffer instead, before the host hands it to
 * whichever backend is active. Backend-agnostic by construction, and it is
 * the same place a host already draws an FPS counter.
 *
 * Nearest-neighbour and integer-stepped on purpose. The panels are authored
 * at exactly 2x the SNES field (512x448 over 256x224), so the common case is
 * an exact halving with no resampling artefacts; anything else lands on the
 * nearest source pixel rather than inventing colours between them. */
void snes_ovl_blit_panel(uint8_t *dst, int pitch, int dst_w, int dst_h,
                         const uint32_t *panel, int panel_w, int panel_h)
{
    if (panel_w <= 0 || panel_h <= 0 || dst_w <= 0 || dst_h <= 0)
        return;
    /* Largest whole-pixel fit, then centre what is left over. */
    int out_w = dst_w * panel_h < dst_h * panel_w ? dst_w : dst_h * panel_w / panel_h;
    int out_h = out_w * panel_h / panel_w;
    if (out_h > dst_h) {
        out_h = dst_h;
        out_w = out_h * panel_w / panel_h;
    }
    snes_ovl_blit_panel_rect(dst, pitch, dst_w, dst_h, panel, panel_w, panel_h,
                             (dst_w - out_w) / 2, (dst_h - out_h) / 2,
                             out_w, out_h);
}

void snes_ovl_upscale_frame(uint8_t *dst, int pitch, int dst_w, int dst_h,
                            const uint32_t *src, int src_pitch,
                            int src_w, int src_h)
{
    if (!dst || !src || pitch <= 0 || src_pitch <= 0 ||
        dst_w <= 0 || dst_h <= 0 || src_w <= 0 || src_h <= 0)
        return;
    for (int y = 0; y < dst_h; y++) {
        const uint32_t *src_row =
            (const uint32_t *)((const uint8_t *)src +
                               (size_t)(y * src_h / dst_h) * (size_t)src_pitch);
        uint32_t *dst_row = (uint32_t *)(dst + (size_t)y * (size_t)pitch);
        for (int x = 0; x < dst_w; x++)
            dst_row[x] = src_row[x * src_w / dst_w];
    }
}

void snes_ovl_blit_panel_rect(uint8_t *dst, int pitch, int dst_w, int dst_h,
                              const uint32_t *panel, int panel_w, int panel_h,
                              int rx, int ry, int rw, int rh)
{
    if (!dst || !panel || pitch <= 0 ||
        dst_w <= 0 || dst_h <= 0 || panel_w <= 0 || panel_h <= 0 ||
        rw <= 0 || rh <= 0)
        return;

    const int out_w = rw, out_h = rh;
    const int ox = rx, oy = ry;

    for (int y = 0; y < out_h; y++) {
        const int dy = oy + y;
        if (dy < 0 || dy >= dst_h)
            continue;                    /* clip rather than scribble */
        const uint32_t *src_row = panel + (size_t)(y * panel_h / out_h) * panel_w;
        uint32_t *dst_row = (uint32_t *)(dst + (size_t)dy * (size_t)pitch);
        for (int x = 0; x < out_w; x++) {
            const int dx = ox + x;
            if (dx < 0 || dx >= dst_w)
                continue;
            const uint32_t s = src_row[x * panel_w / out_w];
            const uint32_t a = s >> 24;
            if (a == 0)
                continue;            /* fully transparent: leave the game */
            if (a == 0xFFu) {
                dst_row[dx] = s;     /* the common case: opaque panel */
                continue;
            }
            const uint32_t d = dst_row[dx];
            const uint32_t na = 255u - a;
            const uint32_t r = (((s >> 16) & 0xFFu) * a + ((d >> 16) & 0xFFu) * na) / 255u;
            const uint32_t g = (((s >>  8) & 0xFFu) * a + ((d >>  8) & 0xFFu) * na) / 255u;
            const uint32_t b = (((s      ) & 0xFFu) * a + ((d      ) & 0xFFu) * na) / 255u;
            dst_row[dx] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
}
