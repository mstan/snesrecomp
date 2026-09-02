/* Synthetic regression for PpuSetWidescreenLayerWorldMirrorBand
 * (WIDESCREEN_PATTERNS P2b: bounded arenas reflect about the authored world
 * edge, not the viewport edge).
 *
 * The scene is a GWED-shaped Mode 1 BG1: a 64x64 tilemap authored only over
 * map px [64, 448), tile 0 either side, with the camera clamped so that the
 * authentic 256 columns never leave the authored span and only the widescreen
 * margins can. No game ROM, generated code, or platform frontend is needed.
 *
 * The load-bearing assertions:
 *   1. every pixel whose tilemap X is inside the authored world renders
 *      byte-identically to the no-policy render (this is what a viewport-space
 *      mirror gets wrong mid-scroll: it discards real authored margin art);
 *   2. every pixel outside it equals the reflected source column;
 *   3. at the camera walls the outermost margin column is non-blank, and
 *      differs from what a viewport-space fold would have produced;
 *   4. with no band configured the wide output is unchanged, and clamp /
 *      mirror bands still win over a world band that overlaps them.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snes/ppu.h"
#include "snes/snes.h"

Snes *g_snes;

/* ppu.c reaches runtime globals defined in snes.c / common_rtl.c and the
 * ws_shadow streaming-tilemap helpers. This harness links ppu.c on its own;
 * none of them participate in the world-mirror policy. */
int snes_frame_counter;
unsigned char g_snesrecomp_last_hdmaen;

uint16_t WsShadowTile(int layer, int screen_x, uint32_t wrapped_y,
                      uint16_t hscroll, uint16_t map_word_offset,
                      uint16_t real_tile) {
    (void)layer; (void)screen_x; (void)wrapped_y; (void)hscroll;
    (void)map_word_offset;
    return real_tile;
}
bool WsShadowLayerActive(int layer) { (void)layer; return false; }
uint32_t WsShadowWorldX(int layer) { (void)layer; return 0; }
uint32_t WsShadowPresentWorldY(int layer, int screen_x) {
    (void)layer; (void)screen_x; return 0;
}
uint32_t WsShadowScrollY(int layer) { (void)layer; return 0; }
void WsShadowOnVramWrite(uint16_t word_adr, uint16_t value) {
    (void)word_adr; (void)value;
}

enum {
    kExtra = 43,                 /* GWED's 16:9 margin, per side */
    kWide = kPpuXPixels + kExtra * 2,
    kWorldLeft = 64,             /* first authored map pixel */
    kWorldRight = 448,           /* one past the last authored map pixel */
    kLine = 1,                   /* ppu_runLine(1) renders framebuffer row 0 */
    kMapBaseWords = 0x0000,
    kCharBaseWords = 0x1000,
};

static int failures;

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

/* Write one 4bpp tile row. Pixel 0 is leftmost (plane bit 7), matching the
 * hardware bit order the renderer decodes. */
static void put_tile_row(Ppu *ppu, int tile, int row, const int *px) {
    uint16_t w0 = 0, w1 = 0;
    for (int p = 0; p < 8; p++) {
        int v = px[p] & 0xf;
        int bit = 7 - p;
        w0 |= (uint16_t)(((v >> 0) & 1) << bit);
        w0 |= (uint16_t)(((v >> 1) & 1) << (bit + 8));
        w1 |= (uint16_t)(((v >> 2) & 1) << bit);
        w1 |= (uint16_t)(((v >> 3) & 1) << (bit + 8));
    }
    ppu->vram[(kCharBaseWords + tile * 16 + row) & 0x7fff] = w0;
    ppu->vram[(kCharBaseWords + tile * 16 + 8 + row) & 0x7fff] = w1;
}

/* A 64x64 Mode 1 BG1 whose tilemap holds art only in map px [64,448). Every
 * authored 8px column gets its own tile, so a pixel identifies its source
 * column and a reflection cannot be confused with a fold. */
static void setup_arena_bg1(Ppu *ppu) {
    ppu->inidisp = 0x0f;
    ppu->bgmode = 1;
    ppu->screenEnabled[0] = 0x01;   /* BG1 on the main screen only */
    ppu->screenEnabled[1] = 0x00;
    ppu->screenWindowed[0] = 0x00;
    ppu->screenWindowed[1] = 0x00;
    ppu->bgXsc[0] = 0x03;           /* map at word 0, 64x64 */
    ppu->bgTileAdr = 0x0001;        /* BG1 char base = word 0x1000 */
    ppu->vScroll[0] = 0;

    memset(ppu->vram, 0, sizeof ppu->vram);
    ppu->cgram[0] = 0x0000;
    for (int i = 1; i < 16; i++)
        ppu->cgram[i] = (uint16_t)(0x0421 * i);

    /* 48 authored tile columns, tiles 1..48. Tile n's rows are a shifted ramp
     * so both the tile index and the pixel position are recoverable. */
    for (int t = 1; t <= 48; t++) {
        for (int row = 0; row < 8; row++) {
            int px[8];
            for (int p = 0; p < 8; p++)
                px[p] = ((t * 3 + p * 5 + row) % 15) + 1;
            put_tile_row(ppu, t, row, px);
        }
    }
    /* Tile 0 stays all-zero: transparent, exactly like GWED's unauthored
     * tilemap columns. */
    for (int scr = 0; scr < 2; scr++) {
        for (int row = 0; row < 32; row++) {
            for (int col = 0; col < 32; col++) {
                int tx = scr * 32 + col;         /* 0..63 */
                int tile = 0;
                if (tx >= kWorldLeft / 8 && tx < kWorldRight / 8)
                    tile = tx - kWorldLeft / 8 + 1;
                ppu->vram[kMapBaseWords + scr * 0x400 + row * 32 + col] =
                    (uint16_t)tile;
            }
        }
    }
}

static void render_wide(Ppu *ppu, uint32_t *out, int hscroll,
                        bool world_band, bool clamp_band, bool mirror_band) {
    memset(out, 0, sizeof(uint32_t) * kWide);
    ppu_reset(ppu);
    PpuBeginDrawing(ppu, (uint8_t *)out, sizeof(uint32_t) * kWide,
                    kPpuRenderFlags_NewRenderer);
    setup_arena_bg1(ppu);
    PpuSetExtraSpace(ppu, kExtra);   /* resets layer policies: must come first */
    if (clamp_band)
        PpuSetWidescreenLayerClampBand(ppu, 0, 0, 225);
    if (mirror_band)
        PpuSetWidescreenLayerMirror(ppu, 0x01);
    if (world_band)
        PpuSetWidescreenLayerWorldMirrorBand(ppu, 0, 0, 225, kWorldLeft,
                                             kWorldRight);
    ppu->hScroll[0] = (uint16_t)hscroll;
    ppu_runLine(ppu, 0);
    ppu->hScroll[0] = (uint16_t)hscroll;
    ppu_runLine(ppu, kLine);
}

/* One camera position: compare the world-mirrored render against the
 * unpolicied one, pixel by pixel, using the reflection rule as the oracle. */
static void check_camera(Ppu *ppu, int hscroll, const char *label,
                         bool expect_out_of_world) {
    uint32_t *base = malloc(sizeof(uint32_t) * kWide);
    uint32_t *out = malloc(sizeof(uint32_t) * kWide);
    if (!base || !out) { fprintf(stderr, "FAIL: oom\n"); failures++; return; }

    render_wide(ppu, base, hscroll, false, false, false);
    render_wide(ppu, out, hscroll, true, false, false);

    const int a = kWorldLeft - hscroll;    /* screen x of the world's left edge */
    const int b = kWorldRight - hscroll;   /* screen x one past its right edge */
    int inside_mismatch = 0, reflected_mismatch = 0, reflected_pixels = 0;
    int viewport_fold_differs = 0;
    char msg[160];

    for (int x = -kExtra; x < kPpuXPixels + kExtra; x++) {
        uint32_t got = out[x + kExtra];
        if (x >= a && x < b) {
            if (got != base[x + kExtra])
                inside_mismatch++;
            continue;
        }
        int sx = (x < a) ? 2 * a - 1 - x : 2 * b - 1 - x;
        if (sx < a || sx >= b || sx < -kExtra || sx >= kPpuXPixels + kExtra) {
            if (got != 0)
                reflected_mismatch++;   /* nothing to sample: stays backdrop */
            continue;
        }
        reflected_pixels++;
        if (got != base[sx + kExtra])
            reflected_mismatch++;
        /* What PpuSetWidescreenLayerMirror would have shown here. */
        int fold = (x < 0) ? -x : (2 * kPpuXPixels - 2 - x);
        if (fold >= -kExtra && fold < kPpuXPixels + kExtra &&
            base[fold + kExtra] != got)
            viewport_fold_differs++;
    }

    snprintf(msg, sizeof msg,
             "%s: in-world columns render naturally (byte-identical)", label);
    check(inside_mismatch == 0, msg);
    snprintf(msg, sizeof msg,
             "%s: out-of-world columns equal the reflected source", label);
    check(reflected_mismatch == 0, msg);
    if (expect_out_of_world) {
        snprintf(msg, sizeof msg, "%s: some columns leave the world", label);
        check(reflected_pixels > 0, msg);
        snprintf(msg, sizeof msg,
                 "%s: outermost margin column is non-blank", label);
        check(a > -kExtra ? out[0] != 0 : out[kWide - 1] != 0, msg);
        snprintf(msg, sizeof msg,
                 "%s: world reflection differs from a viewport fold", label);
        check(viewport_fold_differs > 0, msg);
    } else {
        snprintf(msg, sizeof msg,
                 "%s: nothing leaves the world, so nothing is synthesized",
                 label);
        check(reflected_pixels == 0, msg);
        snprintf(msg, sizeof msg, "%s: output identical to no policy at all",
                 label);
        check(memcmp(base, out, sizeof(uint32_t) * kWide) == 0, msg);
    }
    free(base);
    free(out);
}

int main(void) {
    Ppu *ppu = ppu_init();
    if (!ppu) return 2;

    /* Left wall: the camera cannot scroll further left, so the whole left
     * margin is off the end of the authored tilemap. */
    check_camera(ppu, 64, "left wall (hScroll 64)", true);
    /* Right wall: mirror image of the above. */
    check_camera(ppu, 192, "right wall (hScroll 192)", true);
    /* Mid-scroll: both margins hold genuine authored art. This is the case a
     * viewport-space mirror or repeat band destroys. */
    check_camera(ppu, 128, "mid-scroll (hScroll 128)", false);

    /* Precedence: a clamp band and a layer mirror each keep the layer out of
     * the margins, so a world band overlapping them must be inert -- the
     * output has to match the clamp/mirror configuration on its own. */
    {
        uint32_t *a = malloc(sizeof(uint32_t) * kWide);
        uint32_t *b = malloc(sizeof(uint32_t) * kWide);
        if (a && b) {
            render_wide(ppu, a, 64, false, true, false);
            render_wide(ppu, b, 64, true, true, false);
            check(memcmp(a, b, sizeof(uint32_t) * kWide) == 0,
                  "clamp band wins over an overlapping world band");
            render_wide(ppu, a, 64, false, false, true);
            render_wide(ppu, b, 64, true, false, true);
            check(memcmp(a, b, sizeof(uint32_t) * kWide) == 0,
                  "layer mirror wins over an overlapping world band");
        } else {
            fprintf(stderr, "FAIL: oom\n");
            failures++;
        }
        free(a);
        free(b);
    }

    /* Inert at authentic width: configuring a world band with no margin must
     * not change the 256-wide picture. */
    {
        uint32_t native[kPpuXPixels], banded[kPpuXPixels];
        for (int pass = 0; pass < 2; pass++) {
            uint32_t *dst = pass ? banded : native;
            memset(dst, 0, sizeof native);
            ppu_reset(ppu);
            PpuBeginDrawing(ppu, (uint8_t *)dst,
                            sizeof(uint32_t) * kPpuXPixels,
                            kPpuRenderFlags_NewRenderer);
            setup_arena_bg1(ppu);
            if (pass)
                PpuSetWidescreenLayerWorldMirrorBand(ppu, 0, 0, 225,
                                                     kWorldLeft, kWorldRight);
            ppu->hScroll[0] = 64;
            ppu_runLine(ppu, 0);
            ppu->hScroll[0] = 64;
            ppu_runLine(ppu, kLine);
        }
        check(memcmp(native, banded, sizeof native) == 0,
              "world band is inert at authentic 256 width");
    }

    /* The disable form must clear the slot rather than leave it half set. */
    {
        ppu_reset(ppu);
        PpuSetExtraSpace(ppu, kExtra);
        PpuSetWidescreenLayerWorldMirrorBand(ppu, 0, 0, 225, kWorldLeft,
                                             kWorldRight);
        check(PpuWidescreenLayerWorldBandAt(ppu, 0, kLine) == 0,
              "a configured band is reported for its scanlines");
        PpuSetWidescreenLayerWorldMirrorBand(ppu, 0, 0, 0, kWorldLeft,
                                             kWorldRight);
        check(PpuWidescreenLayerWorldBandAt(ppu, 0, kLine) < 0,
              "an empty band disables the slot");
        PpuSetWidescreenLayerWorldMirrorBandSlot(ppu, 1, 0, 0, 225,
                                                 kWorldRight, kWorldLeft);
        check(PpuWidescreenLayerWorldBandAt(ppu, 0, kLine) < 0,
              "an inverted world span disables the slot");
        PpuSetWidescreenLayerWorldMirrorBandSlot(ppu, 1, 0, 100, 200,
                                                 kWorldLeft, kWorldRight);
        check(PpuWidescreenLayerWorldBandAt(ppu, 0, 150) == 1 &&
              PpuWidescreenLayerWorldBandAt(ppu, 0, 50) < 0,
              "slot 1 is independently addressable");
    }

    ppu_free(ppu);
    printf("ppu_world_mirror_test: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
