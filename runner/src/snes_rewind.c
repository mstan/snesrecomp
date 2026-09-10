/*
 * snes_rewind.c — a ring of whole-machine snapshots, and a filmstrip.
 *
 * A snapshot is RtlSaveSnapshotToMemory() -- the SAME thing a save-state file
 * holds, minus the file. Deliberately not a hand-rolled snes_saveload() sink:
 * that would capture the machine but skip everything the real path wraps
 * around it -- the magic/version header, the APU lock, and the game's own
 * state_save_extra, which on a widescreen port is where fields that sit
 * outside ppu_saveload live. A rewind that restored 99% of the machine would
 * be worse than no rewind, because the missing 1% would look like a game bug.
 *
 * The ring sizes ITSELF from the first snapshot rather than a hardcoded
 * guess, which would either waste memory or silently truncate the day the
 * save format grows a field.
 */

#include "snes_rewind.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snes_overlay_draw.h"
#include "common_rtl.h"
#include "netplay/snes_netplay.h"

/* Thumbnail geometry matches the save-state menu so the two overlays look
 * like one product rather than two features. */
#define RW_THUMB_W 128
#define RW_THUMB_H 112

#define RW_DEPTH_DEFAULT   60
#define RW_DEPTH_MIN        4
#define RW_DEPTH_MAX      240
#define RW_INTERVAL_DEFAULT 6
#define RW_INTERVAL_MIN     1
#define RW_INTERVAL_MAX    60

typedef struct {
    uint8_t *blob;                     /* one whole-machine snapshot */
    size_t   len;
    size_t   cap;
    uint32_t thumb[RW_THUMB_W * RW_THUMB_H];
    int      have_thumb;
    int      valid;
} RwSlot;

static int      s_configured;
static int      s_enabled = 1;
static int      s_depth = RW_DEPTH_DEFAULT;
static int      s_interval = RW_INTERVAL_DEFAULT;
static size_t   s_blob_size;           /* learned from the first capture */

static RwSlot  *s_ring;
static int      s_head;                /* next slot to write */
static int      s_count;               /* live slots, <= s_depth */
static int      s_frame_tick;

static int      s_open;
static int      s_sel;                 /* 0 = newest, grows going back */

static uint32_t s_live_thumb[RW_THUMB_W * RW_THUMB_H];
static int      s_have_live_thumb;

/* ---- env ---------------------------------------------------------------- */

static int env_int(const char *name, int def, int lo, int hi) {
    const char *v = getenv(name);
    int n;
    if (!v || !v[0]) return def;
    n = atoi(v);
    if (n < lo) n = lo;
    if (n > hi) n = hi;
    return n;
}

void snes_rewind_configure(void) {
    const char *off;
    if (s_configured) return;
    s_configured = 1;

    off = getenv("SNESRECOMP_REWIND");
    if (off && off[0] == '0') { s_enabled = 0; return; }

    s_depth = env_int("SNESRECOMP_REWIND_DEPTH", RW_DEPTH_DEFAULT,
                      RW_DEPTH_MIN, RW_DEPTH_MAX);
    s_interval = env_int("SNESRECOMP_REWIND_INTERVAL", RW_INTERVAL_DEFAULT,
                         RW_INTERVAL_MIN, RW_INTERVAL_MAX);

    s_ring = (RwSlot *)calloc((size_t)s_depth, sizeof(RwSlot));
    if (!s_ring) {
        /* Out of memory is a reason to not have rewind, not a reason to die.
         * Say so once: a silently absent feature is a support question. */
        fprintf(stderr, "[snes_rewind] could not allocate %d slots; disabled\n",
                s_depth);
        s_enabled = 0;
        return;
    }
    fprintf(stderr, "[snes_rewind] %d snapshots every %d frames (%.1fs of history)\n",
            s_depth, s_interval, (double)s_depth * s_interval / 60.0);
}

void snes_rewind_shutdown(void) {
    int i;
    if (s_ring) {
        for (i = 0; i < s_depth; i++) free(s_ring[i].blob);
        free(s_ring);
        s_ring = NULL;
    }
    s_count = s_head = s_sel = 0;
    s_open = 0;
    s_configured = 0;
}

int snes_rewind_enabled(void) { return s_enabled && s_ring != NULL; }

/* ---- capture ------------------------------------------------------------ */

/* Generous ceiling for the first probe; the real size replaces it. */
#define RW_PROBE_CAP (2u * 1024u * 1024u)

static void capture(void) {
    RwSlot *slot;
    size_t n;

    if (!s_blob_size) {
        uint8_t *probe = (uint8_t *)malloc(RW_PROBE_CAP);
        if (!probe) return;
        n = RtlSaveSnapshotToMemory(probe, RW_PROBE_CAP);
        free(probe);
        if (!n) return;                /* machine not ready yet; try later */
        s_blob_size = n;
        fprintf(stderr, "[snes_rewind] snapshot is %zu bytes; ring is %.1f MB\n",
                s_blob_size,
                (double)s_blob_size * (double)s_depth / (1024.0 * 1024.0));
    }

    slot = &s_ring[s_head];
    if (!slot->blob) {
        /* Headroom: a snapshot can legitimately differ in size between
         * frames (the game's extra chunk is not fixed-width), and a capture
         * that does not fit is a capture lost. */
        slot->cap = s_blob_size + (s_blob_size / 8) + 4096;
        slot->blob = (uint8_t *)malloc(slot->cap);
        if (!slot->blob) { slot->cap = 0; return; }
    }

    n = RtlSaveSnapshotToMemory(slot->blob, slot->cap);
    if (!n) {
        /* Did not fit, or the machine refused. Record nothing rather than a
         * partial snapshot: restoring one would put the machine into a state
         * that never existed. */
        slot->valid = 0;
        return;
    }
    slot->len = n;
    slot->valid = 1;
    slot->have_thumb = s_have_live_thumb;
    if (s_have_live_thumb)
        memcpy(slot->thumb, s_live_thumb, sizeof(slot->thumb));

    s_head = (s_head + 1) % s_depth;
    if (s_count < s_depth) s_count++;
}

void snes_rewind_note_frame(void) {
    if (!snes_rewind_enabled() || s_open) return;
    /* Rewinding is one machine moving its own clock backwards. Two peers
     * cannot do that independently and still agree, so there is nothing to
     * record during a session. */
    if (snes_netplay_active()) return;
    if (++s_frame_tick < s_interval) return;
    s_frame_tick = 0;
    capture();
}

void snes_rewind_note_framebuffer(const uint32_t *fb, int width, int height) {
    int x, y;
    if (!snes_rewind_enabled() || !fb || width <= 0 || height <= 0 || s_open)
        return;
    for (y = 0; y < RW_THUMB_H; y++) {
        const uint32_t *row = fb + (size_t)(y * height / RW_THUMB_H) * (size_t)width;
        uint32_t *out = s_live_thumb + (size_t)y * RW_THUMB_W;
        for (x = 0; x < RW_THUMB_W; x++)
            out[x] = row[x * width / RW_THUMB_W] | 0xFF000000u;
    }
    s_have_live_thumb = 1;
}

/* ---- selection ---------------------------------------------------------- */

/* sel 0 is the newest snapshot; sel grows going back in time. */
static RwSlot *slot_at(int sel) {
    int idx;
    if (sel < 0 || sel >= s_count) return NULL;
    idx = s_head - 1 - sel;
    while (idx < 0) idx += s_depth;
    return &s_ring[idx];
}

int snes_rewind_open(void) {
    if (!snes_rewind_enabled() || snes_netplay_active()) return 0;
    if (s_count < 2) return 0;         /* nothing to go back to */
    s_open = 1;
    s_sel = 0;
    return 1;
}

int snes_rewind_is_open(void) { return s_open; }

void snes_rewind_step(int dir) {
    if (!s_open) return;
    /* Clamp rather than wrap: wrapping from the oldest snapshot round to the
     * newest would silently move the player FORWARDS, which is not what any
     * press of "further back" can reasonably mean. */
    s_sel += (dir < 0) ? 1 : -1;
    if (s_sel < 0) s_sel = 0;
    if (s_sel >= s_count) s_sel = s_count - 1;
}

float snes_rewind_selected_seconds(void) {
    return (float)s_sel * (float)s_interval / 60.0f;
}

void snes_rewind_commit(void) {
    RwSlot *slot = slot_at(s_sel);
    if (!s_open) return;
    s_open = 0;
    if (!slot || !slot->valid) return;
    if (!RtlLoadSnapshotFromMemory(slot->blob, slot->len)) {
        /* Refused (bad magic, version drift, short read). The machine is
         * untouched -- the loader validates before it writes anything -- so
         * the honest move is to leave the player where they were. */
        fprintf(stderr, "[snes_rewind] snapshot %d refused; staying put\n", s_sel);
        return;
    }

    /* Drop everything after the point we landed on. The machine is there now,
     * and keeping snapshots from the timeline just abandoned would let the
     * next rewind travel to a future that no longer happens. */
    s_count -= s_sel;
    s_head = s_head - s_sel;
    while (s_head < 0) s_head += s_depth;
    s_sel = 0;
    s_frame_tick = 0;
}

void snes_rewind_close(void) { s_open = 0; s_sel = 0; }

/* ---- filmstrip ---------------------------------------------------------- */

/*
 * A strip of thumbnails along the bottom, newest on the right, with the
 * selection boxed. Sized to the SNES frame so the host can blit it over the
 * game rect with the same destination it already computes.
 */
#define RW_STRIP_W   512
#define RW_STRIP_H   176   /* caption 34 + thumbs 63 + hint row 32 + margins */
#define RW_CELL_W     72
#define RW_CELL_H     63
#define RW_CELL_GAP    6
#define RW_CAPTION_Y  10

static uint32_t s_strip[RW_STRIP_W * RW_STRIP_H];

/* Text and face buttons come from snes_overlay_draw.c.
 *
 * The comment that used to sit here said the 8x8 font lived only in
 * snes_osd.c and that a plain progress bar was worth avoiding a second copy.
 * That was already untrue -- snes_savestate_menu.c had its own FONT8 -- so
 * the strip was doing without a caption to avoid a duplication that had
 * happened anyway. The font now lives in one place and all three overlays
 * share it. */
static void strip_fill(int x0, int y0, int w, int h, uint32_t col) {
    int x, y;
    for (y = y0; y < y0 + h; y++) {
        if (y < 0 || y >= RW_STRIP_H) continue;
        for (x = x0; x < x0 + w; x++) {
            if (x < 0 || x >= RW_STRIP_W) continue;
            s_strip[y * RW_STRIP_W + x] = col;
        }
    }
}

static void strip_frame(int x0, int y0, int w, int h, uint32_t col, int t) {
    strip_fill(x0, y0, w, t, col);
    strip_fill(x0, y0 + h - t, w, t, col);
    strip_fill(x0, y0, t, h, col);
    strip_fill(x0 + w - t, y0, t, h, col);
}

static void strip_thumb(const uint32_t *thumb, int x0, int y0, int w, int h) {
    int x, y;
    for (y = 0; y < h; y++) {
        const uint32_t *row = thumb + (size_t)(y * RW_THUMB_H / h) * RW_THUMB_W;
        for (x = 0; x < w; x++) {
            const int dx = x0 + x, dy = y0 + y;
            if (dx < 0 || dx >= RW_STRIP_W || dy < 0 || dy >= RW_STRIP_H) continue;
            s_strip[dy * RW_STRIP_W + dx] = row[x * RW_THUMB_W / w] | 0xFF000000u;
        }
    }
}

/* Title and how far back the selection is, over a bar tracking the same thing.
 * Full bar = the whole ring. */
static void strip_caption(void) {
    const int track_x = 12, track_w = RW_STRIP_W - 24, track_h = 4;
    const int bar_y = RW_CAPTION_Y + 20;
    char buf[48];
    float secs = snes_rewind_selected_seconds();
    int fill, whole, tenths;

    snes_ovl_draw_text(s_strip, RW_STRIP_W, RW_STRIP_H,
                       12, RW_CAPTION_Y, "REWIND", 0xFFFFD24Du, 2);

    /* -1.2s, formatted without printf's float support: some runner targets
     * build against a minimal libc where %f is not linked in. */
    whole = (int)secs;
    tenths = (int)((secs - (float)whole) * 10.0f + 0.5f);
    if (tenths > 9) { whole += 1; tenths = 0; }
    snprintf(buf, sizeof(buf), "-%d.%dS", whole, tenths);
    snes_ovl_draw_text(s_strip, RW_STRIP_W, RW_STRIP_H,
                       RW_STRIP_W - 12 - (int)strlen(buf) * 16,
                       RW_CAPTION_Y, buf, 0xFFE2E5EBu, 2);

    strip_fill(track_x, bar_y, track_w, track_h, 0xFF303030u);
    if (s_count > 1) {
        fill = track_w - (int)((long)track_w * s_sel / (s_count - 1));
        if (fill < 2) fill = 2;
        strip_fill(track_x, bar_y, fill, track_h, 0xFFE0E0E0u);
    }
}

/* Face-button glyphs and nothing else.
 *
 * This started as a copy of the save-state menu's hint row: two glyph+label
 * pairs, then "LEFT RIGHT SCRUB", then a line of keyboard equivalents. In the
 * filmstrip that reads as clutter -- the strip is one horizontal row of
 * thumbnails with an obvious direction, so naming the directions explains
 * nothing, and the keyboard line repeats what the glyphs already say. Only the
 * two actions a player has to be told survive, and they get the whole band. */
static void strip_hints(void) {
    const int band_y = RW_STRIP_H - 44;
    const int band_h = RW_STRIP_H - band_y;
    const int btn_y  = band_y + (band_h - 18) / 2;

    snes_ovl_fill_rect(s_strip, RW_STRIP_W, RW_STRIP_H,
                       0, band_y, RW_STRIP_W, band_h, 0xFF171B25u);
    snes_ovl_draw_button(s_strip, RW_STRIP_W, RW_STRIP_H,
                         12, btn_y, 'A', SNES_OVL_COL_A);
    snes_ovl_draw_text(s_strip, RW_STRIP_W, RW_STRIP_H,
                       36, btn_y + 5, "SELECT", 0xFFE2E5EBu, 1);
    snes_ovl_draw_button(s_strip, RW_STRIP_W, RW_STRIP_H,
                         120, btn_y, 'B', SNES_OVL_COL_B);
    snes_ovl_draw_text(s_strip, RW_STRIP_W, RW_STRIP_H,
                       144, btn_y + 5, "BACK", 0xFFE2E5EBu, 1);
}

int snes_rewind_overlay_image(const uint32_t **pixels, int *w, int *h) {
    int i, visible, first, x;
    /* Below the caption: title text (8px cell x2 scale = 16) plus the bar at
     * +20 and 4 tall, so 34 is the first free row. */
    const int strip_y = RW_CAPTION_Y + 32;

    if (!s_open) {
        if (pixels) *pixels = NULL;
        if (w) *w = 0;
        if (h) *h = 0;
        return 0;
    }

    for (i = 0; i < RW_STRIP_W * RW_STRIP_H; i++)
        s_strip[i] = 0xE0101014u;      /* translucent-looking dark panel */

    strip_caption();
    strip_hints();

    /* Show a window of cells centred on the selection, newest to the right. */
    visible = (RW_STRIP_W - 24) / (RW_CELL_W + RW_CELL_GAP);
    if (visible < 1) visible = 1;
    first = s_sel - visible / 2;
    if (first < 0) first = 0;
    if (first + visible > s_count) first = s_count - visible;
    if (first < 0) first = 0;

    x = 12;
    for (i = 0; i < visible && first + i < s_count; i++) {
        /* Rightmost cell is the newest, so draw the window in reverse. */
        const int sel = first + (visible - 1 - i);
        const RwSlot *slot = (sel >= 0 && sel < s_count) ? slot_at(sel) : NULL;
        const int cx = x + i * (RW_CELL_W + RW_CELL_GAP);
        if (!slot) continue;
        if (slot->have_thumb)
            strip_thumb(slot->thumb, cx, strip_y, RW_CELL_W, RW_CELL_H);
        else
            strip_fill(cx, strip_y, RW_CELL_W, RW_CELL_H, 0xFF202028u);
        strip_frame(cx - 1, strip_y - 1, RW_CELL_W + 2, RW_CELL_H + 2,
                    (sel == s_sel) ? 0xFFFFFFFFu : 0xFF404048u,
                    (sel == s_sel) ? 2 : 1);
    }

    if (pixels) *pixels = s_strip;
    if (w) *w = RW_STRIP_W;
    if (h) *h = RW_STRIP_H;
    return 1;
}
