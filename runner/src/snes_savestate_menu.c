/*
 * snes_savestate_menu.c — full-screen save-state slot browser.
 *
 * Ported from psxrecomp's runtime/src/psx_savestate_menu.c. See the header
 * for why the state machine lives here rather than in each host's main.c.
 *
 * Software-rasterized into a plain ARGB buffer, with no dependency on SDL or
 * on any renderer: the host asks for the image and blits it however it draws
 * anything else. The panel is 2x the SNES frame, so a host that presents it
 * with the same destination rect as the game texture gets correct geometry
 * for free.
 *
 * Thumbnails are a sidecar file next to the slot ("<slot>.sav.thumb"), not a
 * chunk inside the savestate. The RTLS blob is versioned and shared with
 * netplay and rollback; growing it to carry a picture would put a cosmetic
 * feature in the path of state compatibility. A missing or stale sidecar
 * costs a grey "NEW" tile and nothing else.
 */

/* localtime_r is POSIX, not C11: a strict -std=c11 translation unit does not
 * see it without this, and the declaration must precede every include. The
 * game hosts happen to build with looser flags, which is exactly why this is
 * stated here rather than left to the consumer's compiler settings. */
#ifndef _WIN32
#  ifndef _DEFAULT_SOURCE
#    define _DEFAULT_SOURCE 1
#  endif
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200809L
#  endif
#endif

#include "snes_savestate_menu.h"

#include "common_rtl.h"
#include "desktop/sdl_compat.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define SSM_W SNES_SSM_W
#define SSM_H SNES_SSM_H
#define SSM_SLOTS SNES_SAVESTATE_SLOTS

#define SSM_THUMB_W 128
#define SSM_THUMB_H 112

#define SSM_VISIBLE_ROWS 3
#define SSM_ROWS_X 20
#define SSM_ROWS_Y 52
#define SSM_ROWS_W 472
#define SSM_ROW_H 106
#define SSM_ROW_GAP 6
#define SSM_TILE_W 116
#define SSM_TILE_H 100

/* Nav auto-repeat, in milliseconds: how long a held direction waits before
 * it starts repeating, and how fast it repeats after that. */
#define SSM_REPEAT_DELAY 350u
#define SSM_REPEAT_RATE  90u

/* Runner input word bits. Documented in the host scaffold's read_keyboard()
 * and in keybinds.h; restated here so this file is readable on its own. */
#define SSM_BTN_B      (1u << 0)
#define SSM_BTN_Y      (1u << 1)
#define SSM_BTN_SELECT (1u << 2)
#define SSM_BTN_START  (1u << 3)
#define SSM_BTN_UP     (1u << 4)
#define SSM_BTN_DOWN   (1u << 5)
#define SSM_BTN_A      (1u << 8)
#define SSM_BTN_X      (1u << 9)
#define SSM_BTN_R      (1u << 11)

#define SSM_OPEN_GESTURE (SSM_BTN_SELECT | SSM_BTN_R)

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

static int s_open;
static int s_selected;
static int s_dirty = 1;
static uint32_t s_panel[SSM_W * SSM_H];
static uint32_t s_thumbs[SSM_SLOTS][SSM_THUMB_W * SSM_THUMB_H];
static int s_have_thumb[SSM_SLOTS];
static int s_thumbs_scanned;

/* The most recent game frame, already downsampled, waiting to become the
 * thumbnail of whatever slot is saved next. */
static uint32_t s_live_thumb[SSM_THUMB_W * SSM_THUMB_H];
static int s_have_live_thumb;

/* Footer status line — this port's substitute for psxrecomp's host_osd_push.
 * The SNES hosts have no OSD layer, and adding one to carry three strings
 * would be a subsystem where a line of text will do. */
static char s_status[48];

/* Held-direction state for auto-repeat. */
static uint32_t s_prev_inputs;
static uint32_t s_repeat_next;
static int s_repeat_dir;

/* Buttons that were still held when the menu closed. They stay masked out of
 * the guest's input word until each one is physically released. Without this,
 * the B that dismissed the menu also lands in the game on the very next
 * frame, and Select+R held through a close re-opens the menu immediately.
 * psxrecomp calls the same idea savestate_input_guard_arm(). */
static uint32_t s_input_guard;

/* ── Rasterizer primitives ─────────────────────────────────────────────── */

static void fill_rect(uint32_t *dst, int x0, int y0, int w, int h, uint32_t col)
{
    int x, y;
    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    if (x0 + w > SSM_W) w = SSM_W - x0;
    if (y0 + h > SSM_H) h = SSM_H - y0;
    if (w <= 0 || h <= 0) return;
    for (y = y0; y < y0 + h; y++)
        for (x = x0; x < x0 + w; x++)
            dst[y * SSM_W + x] = col;
}

static void stroke_rect(uint32_t *dst, int x, int y, int w, int h, uint32_t col)
{
    fill_rect(dst, x, y, w, 1, col);
    fill_rect(dst, x, y + h - 1, w, 1, col);
    fill_rect(dst, x, y, 1, h, col);
    fill_rect(dst, x + w - 1, y, 1, h, col);
}

static void fill_disc(uint32_t *dst, int cx, int cy, int r, uint32_t col)
{
    int x, y;
    const int rr = r * r;
    for (y = -r; y <= r; y++)
        for (x = -r; x <= r; x++)
            if (x * x + y * y <= rr)
                fill_rect(dst, cx + x, cy + y, 1, 1, col);
}

static void draw_char(uint32_t *dst, int x0, int y0, char c,
                      uint32_t col, int scale)
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
                    if ((unsigned)dx < SSM_W && (unsigned)dy < SSM_H)
                        dst[dy * SSM_W + dx] = col;
                }
        }
    }
}

static void draw_text(uint32_t *dst, int x, int y, const char *s,
                      uint32_t col, int scale)
{
    if (!s) return;
    while (*s) {
        draw_char(dst, x, y, *s++, col, scale);
        x += 8 * scale;
    }
}

/* A SNES face button: filled disc with its letter punched out of the middle.
 * The colours are the Super Famicom's, which is the closest thing the SNES
 * has to psxrecomp's shape-coded PlayStation glyphs. */
static void draw_snes_button(uint32_t *dst, int x, int y, char label,
                             uint32_t col)
{
    fill_disc(dst, x + 9, y + 9, 9, col);
    fill_disc(dst, x + 9, y + 9, 8, 0xFF171B25u);
    draw_char(dst, x + 5, y + 5, label, col, 1);
}

static void blit_thumb(uint32_t *dst, int x0, int y0, int w, int h,
                       const uint32_t *src)
{
    int x, y;
    for (y = 0; y < h; y++) {
        int sy = y * SSM_THUMB_H / h;
        for (x = 0; x < w; x++) {
            int sx = x * SSM_THUMB_W / w;
            dst[(y0 + y) * SSM_W + (x0 + x)] =
                src[sy * SSM_THUMB_W + sx] | 0xFF000000u;
        }
    }
}

/* ── Slot files ────────────────────────────────────────────────────────── */

static void slot_path(int slot, char *buf, size_t cap)
{
    RtlSaveSlotPath(slot, buf, cap);
}

static void thumb_path(int slot, char *buf, size_t cap)
{
    char sav[192];
    RtlSaveSlotPath(slot, sav, sizeof(sav));
    snprintf(buf, cap, "%s.thumb", sav);
}

static int slot_exists(int slot)
{
    char path[256];
    struct stat st;
    slot_path(slot, path, sizeof(path));
    return stat(path, &st) == 0 && st.st_size > 0;
}

static int slot_mtime(int slot, time_t *out)
{
    char path[256];
    struct stat st;
    slot_path(slot, path, sizeof(path));
    if (stat(path, &st) != 0 || st.st_size <= 0)
        return 0;
    if (out) *out = st.st_mtime;
    return 1;
}

#define SSM_THUMB_MAGIC 0x42485453u /* "STHB" little-endian */

static int read_thumb(int slot, uint32_t *out)
{
    char path[256];
    FILE *f;
    uint32_t hdr[3];
    size_t px = (size_t)SSM_THUMB_W * SSM_THUMB_H;

    thumb_path(slot, path, sizeof(path));
    f = fopen(path, "rb");
    if (!f)
        return 0;
    if (fread(hdr, sizeof(uint32_t), 3, f) != 3 ||
        hdr[0] != SSM_THUMB_MAGIC ||
        hdr[1] != (uint32_t)SSM_THUMB_W ||
        hdr[2] != (uint32_t)SSM_THUMB_H ||
        fread(out, sizeof(uint32_t), px, f) != px) {
        fclose(f);
        return 0;
    }
    fclose(f);
    return 1;
}

static void write_thumb(int slot, const uint32_t *src)
{
    char path[256];
    FILE *f;
    uint32_t hdr[3];

    thumb_path(slot, path, sizeof(path));
    f = fopen(path, "wb");
    if (!f)
        return;
    hdr[0] = SSM_THUMB_MAGIC;
    hdr[1] = (uint32_t)SSM_THUMB_W;
    hdr[2] = (uint32_t)SSM_THUMB_H;
    fwrite(hdr, sizeof(uint32_t), 3, f);
    fwrite(src, sizeof(uint32_t), (size_t)SSM_THUMB_W * SSM_THUMB_H, f);
    fclose(f);
}

/* Rescan every slot's sidecar. Only on open and after a save, not per frame:
 * this is twelve file opens. */
static void refresh_thumbs(void)
{
    int i;
    for (i = 0; i < SSM_SLOTS; i++)
        s_have_thumb[i] = read_thumb(i, s_thumbs[i]);
    s_thumbs_scanned = 1;
}

static void format_slot_status(int slot, char *out, size_t cap)
{
    time_t mt;
    struct tm tmv;
    if (!out || cap == 0) return;
    if (!slot_mtime(slot, &mt)) {
        snprintf(out, cap, "NEW SLOT");
        return;
    }
#ifdef _WIN32
    localtime_s(&tmv, &mt);
#else
    localtime_r(&mt, &tmv);
#endif
    strftime(out, cap, "%Y-%m-%d %H:%M", &tmv);
}

/* ── Panel ─────────────────────────────────────────────────────────────── */

static void rasterize_panel(void)
{
    int i, first;
    char buf[96];

    for (i = 0; i < SSM_W * SSM_H; i++)
        s_panel[i] = 0xFF0F1118u;

    fill_rect(s_panel, 0, 0, SSM_W, 40, 0xFF171B25u);
    draw_text(s_panel, 20, 12, "SAVE STATES", 0xFFFFD24Du, 2);
    draw_text(s_panel, 384, 16, "SELECT+R MENU", 0xFFB8BDC8u, 1);

    if (!s_thumbs_scanned)
        refresh_thumbs();

    first = s_selected - 1;
    if (first < 0) first = 0;
    if (first > SSM_SLOTS - SSM_VISIBLE_ROWS)
        first = SSM_SLOTS - SSM_VISIBLE_ROWS;
    snprintf(buf, sizeof(buf), "%02d-%02d / %02d",
             first + 1, first + SSM_VISIBLE_ROWS, SSM_SLOTS);
    draw_text(s_panel, 20, 42, buf, 0xFF7F8796u, 1);

    for (i = first; i < first + SSM_VISIBLE_ROWS; i++) {
        int visible = i - first;
        int y = SSM_ROWS_Y + visible * (SSM_ROW_H + SSM_ROW_GAP);
        int sel = (i == s_selected);
        uint32_t bg = sel ? 0xFF2B2830u : 0xFF191D27u;
        uint32_t fg = sel ? 0xFFFFD24Du : 0xFFE2E5EBu;
        uint32_t sub = sel ? 0xFFFFFFFFu : 0xFFB2B8C2u;

        fill_rect(s_panel, SSM_ROWS_X, y, SSM_ROWS_W, SSM_ROW_H, bg);
        stroke_rect(s_panel, SSM_ROWS_X, y, SSM_ROWS_W, SSM_ROW_H,
                    sel ? 0xFFFFD24Du : 0xFF303746u);

        snprintf(buf, sizeof(buf), "SLOT %02d", i + 1);
        draw_text(s_panel, SSM_ROWS_X + 12, y + 14, buf, fg, 1);

        if (s_have_thumb[i]) {
            blit_thumb(s_panel, SSM_ROWS_X + 88, y + 3,
                       SSM_TILE_W, SSM_TILE_H, s_thumbs[i]);
            stroke_rect(s_panel, SSM_ROWS_X + 88, y + 3,
                        SSM_TILE_W, SSM_TILE_H, 0xFF3A4352u);
        } else {
            fill_rect(s_panel, SSM_ROWS_X + 88, y + 3,
                      SSM_TILE_W, SSM_TILE_H, 0xFF242A35u);
            stroke_rect(s_panel, SSM_ROWS_X + 88, y + 3,
                        SSM_TILE_W, SSM_TILE_H, 0xFF3A4352u);
            /* An occupied slot with no sidecar is a state saved before
             * thumbnails existed, or one whose sidecar was deleted. Say
             * which, rather than showing "NEW" over a slot that would
             * overwrite something. */
            draw_text(s_panel, SSM_ROWS_X + 122, y + 46,
                      slot_exists(i) ? "SAVED" : "NEW",
                      slot_exists(i) ? 0xFF9AA3B2u : 0xFF707887u, 1);
        }

        format_slot_status(i, buf, sizeof(buf));
        draw_text(s_panel, SSM_ROWS_X + 222, y + 42, buf, sub, 1);
        if (sel)
            draw_text(s_panel, SSM_ROWS_X + SSM_ROWS_W - 72, y + 84,
                      "SELECT", 0xFFFFD24Du, 1);
    }

    fill_rect(s_panel, 0, 388, SSM_W, SSM_H - 388, 0xFF171B25u);
    draw_snes_button(s_panel, 20, 398, 'A', 0xFF6BE06Bu);
    draw_text(s_panel, 44, 402, "LOAD", 0xFFE2E5EBu, 1);
    draw_snes_button(s_panel, 116, 398, 'X', 0xFF5FA8FFu);
    draw_text(s_panel, 140, 402, "SAVE", 0xFFE2E5EBu, 1);
    draw_snes_button(s_panel, 212, 398, 'B', 0xFFFF6B6Bu);
    draw_text(s_panel, 236, 402, "BACK", 0xFFE2E5EBu, 1);
    draw_text(s_panel, 316, 402, "UP DOWN SLOT", 0xFF7F8796u, 1);
    draw_text(s_panel, 20, 422, "KEYS: ARROWS SLOT  X LOAD  S SAVE  ESC BACK  1-9 JUMP",
              0xFFB8BDC8u, 1);
    if (s_status[0])
        draw_text(s_panel, 20, 436, s_status, 0xFFFFD24Du, 1);

    s_dirty = 0;
}

/* ── State machine ─────────────────────────────────────────────────────── */

static void set_status(const char *fmt, int slot)
{
    snprintf(s_status, sizeof(s_status), fmt, slot + 1);
    s_dirty = 1;
}

static void menu_move(int delta)
{
    s_selected += delta;
    while (s_selected < 0)
        s_selected += SSM_SLOTS;
    while (s_selected >= SSM_SLOTS)
        s_selected -= SSM_SLOTS;
    s_status[0] = '\0';
    s_dirty = 1;
}

static void menu_submit(int save)
{
    if (save) {
        RtlSaveLoad(kSaveLoad_Save, s_selected);
        if (s_have_live_thumb)
            write_thumb(s_selected, s_live_thumb);
        refresh_thumbs();
        /* Deliberately stays open after a save, where psxrecomp closes.
         * The freshly written thumbnail appearing in the row is the only
         * direct evidence a player gets that the state was actually
         * written, and this port had no save-state UI at all before now. */
        set_status("SAVED SLOT %02d", s_selected);
        return;
    }
    if (!slot_exists(s_selected)) {
        set_status("SLOT %02d IS EMPTY", s_selected);
        return;
    }
    RtlSaveLoad(kSaveLoad_Load, s_selected);
    snes_savestate_menu_close();
}

int snes_savestate_menu_is_open(void)
{
    return s_open;
}

void snes_savestate_menu_close(void)
{
    s_open = 0;
    s_repeat_dir = 0;
    s_input_guard = s_prev_inputs;
    s_status[0] = '\0';
    s_dirty = 1;
}

uint32_t snes_savestate_menu_filter_guest_input(uint32_t inputs)
{
    /* A guarded button leaves the guard the moment it is let go, and never
     * re-enters it: the mask only ever shrinks. */
    s_input_guard &= inputs;
    return inputs & ~s_input_guard;
}

int snes_savestate_menu_poll_open(uint32_t inputs)
{
    uint32_t prev = s_prev_inputs;
    s_prev_inputs = inputs;
    if (s_open)
        return 0;
    /* Edge on the pair, not on either button: holding Select through a menu
     * and then tapping R is a real gesture, and so is the reverse. */
    if ((inputs & SSM_OPEN_GESTURE) != SSM_OPEN_GESTURE)
        return 0;
    if ((prev & SSM_OPEN_GESTURE) == SSM_OPEN_GESTURE)
        return 0;
    s_open = 1;
    s_status[0] = '\0';
    s_thumbs_scanned = 0;
    s_repeat_dir = 0;
    s_dirty = 1;
    return 1;
}

void snes_savestate_menu_poll_nav(uint32_t inputs, uint32_t ticks_ms)
{
    uint32_t prev = s_prev_inputs;
    uint32_t pressed;
    int dir;

    if (!s_open) {
        s_prev_inputs = inputs;
        return;
    }
    pressed = inputs & ~prev;
    s_prev_inputs = inputs;

    /* Select+R closes as well as opens, so the gesture is its own toggle and
     * a player who opened it by accident undoes it the same way. */
    if ((pressed & SSM_OPEN_GESTURE) &&
        (inputs & SSM_OPEN_GESTURE) == SSM_OPEN_GESTURE) {
        snes_savestate_menu_close();
        return;
    }
    if (pressed & SSM_BTN_B) {
        snes_savestate_menu_close();
        return;
    }
    if (pressed & SSM_BTN_A) {
        menu_submit(0);
        return;
    }
    if (pressed & SSM_BTN_X) {
        menu_submit(1);
        return;
    }

    dir = (inputs & SSM_BTN_DOWN) ? 1 : (inputs & SSM_BTN_UP) ? -1 : 0;
    if (dir == 0) {
        s_repeat_dir = 0;
        return;
    }
    if (dir != s_repeat_dir) {
        s_repeat_dir = dir;
        s_repeat_next = ticks_ms + SSM_REPEAT_DELAY;
        menu_move(dir);
        return;
    }
    /* Unsigned difference, not `ticks_ms >= s_repeat_next`: SDL_GetTicks
     * wraps at 2^32 ms, and a plain comparison across the wrap would stall
     * the repeat for the rest of the session. */
    if ((uint32_t)(ticks_ms - s_repeat_next) < 0x80000000u) {
        s_repeat_next = ticks_ms + SSM_REPEAT_RATE;
        menu_move(dir);
    }
}

/* Only keys that are NOT part of the runner input word belong here.
 * Navigation, load and save all arrive through snes_savestate_menu_poll_nav,
 * because the hosts map the keyboard onto the same twelve SNES bits the pad
 * uses — arrows are already Up/Down, X is already SNES A, S is already
 * SNES X. Handling those here as well would move two slots per keypress and
 * save twice per keypress. What is left is the two things the SNES pad has
 * no button for: a dedicated cancel, and jumping straight to a slot. */
void snes_savestate_menu_handle_key(int key, int repeat)
{
    int slot = -1;

    if (!s_open || repeat)
        return;
    /* SDLK_* is unsigned under SDL3 and signed under SDL2. Cast at every
     * comparison so neither backend warns and neither one compares wrong. */
    if (key == (int)SDLK_ESCAPE || key == (int)SDLK_BACKSPACE) {
        snes_savestate_menu_close();
        return;
    }
    if (key >= (int)SDLK_1 && key <= (int)SDLK_9)
        slot = key - (int)SDLK_1;
    else if (key == (int)SDLK_0)
        slot = 9;
    else if (key == (int)SDLK_MINUS)
        slot = 10;
    else if (key == (int)SDLK_EQUALS)
        slot = 11;
    if (slot < 0 || slot >= SSM_SLOTS)
        return;
    s_selected = slot;
    s_status[0] = '\0';
    s_dirty = 1;
}

void snes_savestate_menu_note_frame(const uint32_t *fb, int w, int h)
{
    int x, y;
    if (!fb || w <= 0 || h <= 0 || s_open)
        return;
    for (y = 0; y < SSM_THUMB_H; y++) {
        const uint32_t *row = fb + (size_t)(y * h / SSM_THUMB_H) * (size_t)w;
        uint32_t *out = s_live_thumb + (size_t)y * SSM_THUMB_W;
        for (x = 0; x < SSM_THUMB_W; x++)
            out[x] = row[x * w / SSM_THUMB_W] | 0xFF000000u;
    }
    s_have_live_thumb = 1;
}

int snes_savestate_menu_overlay_image(const uint32_t **pixels, int *w, int *h)
{
    if (!s_open) {
        if (pixels) *pixels = NULL;
        if (w) *w = 0;
        if (h) *h = 0;
        return 0;
    }
    if (s_dirty)
        rasterize_panel();
    if (pixels) *pixels = s_panel;
    if (w) *w = SSM_W;
    if (h) *h = SSM_H;
    return 1;
}
