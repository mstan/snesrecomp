/* Synthetic regression for PpuSetWidescreenLayerElasticBand /
 * ...ElasticSplitBandSlot (WIDESCREEN_PATTERNS P2c: a continuous status bar
 * anchors its rigid groups to the widened edges and bridges the gap that
 * opens by stretching only material that can survive being stretched).
 *
 * The scene is a GWED-shaped Mode 1 BG1 status row: one tile row whose 256
 * native columns each carry a UNIQUE colour, so a rendered pixel names the
 * source column it came from and no mapping error can hide. A second variant
 * paints a pair of mirror-image GAUGES -- k filled of n, depleting from the
 * outer end, exactly like GWED's health and boost bars -- so the fill
 * fraction can be measured after the remap.
 *
 * The load-bearing assertions:
 *   1. rigid segments are BYTE-EXACT at their shifted destination (this is
 *      what keeps names, digits, labels and markers pixel-perfect while they
 *      move to the 16:9 edges, and what a whole-line stretch band destroys);
 *   2. the centre group is byte-identical to the unpolicied native render;
 *   3. every destination column equals the documented mapping's source
 *      column, and elastic segments preserve their endpoints;
 *   4. a run of identical source columns stretches to identical output
 *      columns, so a plain-chrome bridge is exactly invisible;
 *   5. a gauge's fill FRACTION survives -- k of n becomes within 1px of
 *      round(k*(n+extra)/n) of n+extra, for EVERY k, and the mirror-image
 *      gauge agrees within a pixel;
 *   6. no destination column in [-extra, 256+extra) is left unpainted, i.e.
 *      the anchoring opens no transparent gap;
 *   7. precedence and inertness: unbanded output is unchanged, the band is
 *      inert at authentic 256 width, clamp/mirror win over it, it wins over
 *      an overlapping world band, the degenerate form renders exactly like a
 *      clamp band, and invalid input clears the slot instead of half-applying.
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
 * none of them participate in the elastic band policy. */
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
    kLine = 1,                   /* ppu_runLine(1) renders framebuffer row 0 */
    kMapBaseWords = 0x0000,
    kCharBaseWords = 0x1000,
    /* GWED's measured fight-HUD gauge row: the frame cap is rigid, the bar
     * interior is elastic, and the TIME pod stays centred. */
    kBarL0 = 8, kBarL1 = 120, kBarR0 = 136, kBarR1 = 248,
    kBarLen = kBarL1 - kBarL0,
};

static int failures;

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

/* Write one 4bpp tile row. Pixel 0 is leftmost (plane bit 7). */
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

/* `column_value(x)` supplies the 4bpp pixel index for native column x; the
 * tile column's palette supplies the high nibble of the CGRAM entry, so a
 * column's final colour is cgram[palette*16 + index]. */
typedef int (*ColumnFn)(int x);

/* Column labelling. A 4bpp layer can show at most 15 indices x 8 palettes and
 * the palette is a per-TILE attribute, so at most 8 tile columns -- 64 native
 * columns -- can be labelled uniquely; 256 unique labels are impossible here.
 * The labelling below is therefore exactly period 64 (index = x mod 8 within
 * the tile, palette = tile column mod 8), which is the widest window a Mode 1
 * BG can distinguish. Consequence, stated plainly: a mapping error of exactly
 * 64, 128 or 192 columns would alias; every smaller error shows. */
static int col_distinct(int x) { return (x % 8) + 1; }

/* Gauge state, set by the caller before rendering. `g_filled` of the left
 * bar's kBarLen columns are filled from the INNER end, so the empty part is
 * at the outer end; the right bar is its mirror image. */
static int g_filled;
static int col_gauge(int x) {
    if (x >= kBarL0 && x < kBarL1)
        return (x >= kBarL1 - g_filled) ? 5 : 1;
    if (x >= kBarR0 && x < kBarR1)
        return (x < kBarR0 + g_filled) ? 5 : 1;
    return 9;   /* rigid chrome and the centre pod: uniform but distinct */
}

/* The documented mapping, restated so the render can be checked against it. */
static int map_src(const PpuWsElasticSeg *s, int x) {
    int sw = s->srcX1 - s->srcX0, dw = s->dstX1 - s->dstX0;
    return s->srcX0 + ((2 * (x - s->dstX0) + 1) * sw) / (2 * dw);
}

static void setup_status_bg1(Ppu *ppu, ColumnFn fn, bool uniform_palette) {
    ppu->inidisp = 0x0f;
    ppu->bgmode = 1;
    ppu->screenEnabled[0] = 0x01;   /* BG1 on the main screen only */
    ppu->screenEnabled[1] = 0x00;
    ppu->screenWindowed[0] = 0x00;  /* GWED disables window clipping here */
    ppu->screenWindowed[1] = 0x00;
    ppu->bgXsc[0] = 0x03;           /* map at word 0, 64x64 */
    ppu->bgTileAdr = 0x0001;        /* BG1 char base = word 0x1000 */
    ppu->vScroll[0] = 0;

    memset(ppu->vram, 0, sizeof ppu->vram);
    memset(ppu->cgram, 0, sizeof ppu->cgram);
    for (int i = 1; i < 256; i++)
        ppu->cgram[i] = (uint16_t)(0x0139 * i + 1);   /* all distinct, non-zero */

    /* 32 tiles, one per native tile column, each painted from `fn`. */
    for (int t = 0; t < 32; t++) {
        for (int row = 0; row < 8; row++) {
            int px[8];
            for (int p = 0; p < 8; p++)
                px[p] = fn(t * 8 + p);
            put_tile_row(ppu, t + 1, row, px);
        }
    }
    for (int scr = 0; scr < 2; scr++)
        for (int row = 0; row < 32; row++)
            for (int col = 0; col < 32; col++) {
                int pal = uniform_palette ? 0 : (col & 7);
                ppu->vram[kMapBaseWords + scr * 0x400 + row * 32 + col] =
                    (uint16_t)((col + 1) | (pal << 10));
            }
}

enum PolicyKind {
    kNoPolicy = 0,
    kSplitBand,        /* the GWED gauge-row split */
    kClampOnly,
    kClampAndSplit,
    kMirrorOnly,
    kMirrorAndSplit,
    kWorldAndSplit,
    kIdentityBand,     /* the degenerate "clamp" form of the split builder */
};

static void render_wide(Ppu *ppu, uint32_t *out, ColumnFn fn,
                        enum PolicyKind kind) {
    const bool uniform_palette = (fn == col_gauge);
    memset(out, 0, sizeof(uint32_t) * kWide);
    ppu_reset(ppu);
    PpuBeginDrawing(ppu, (uint8_t *)out, sizeof(uint32_t) * kWide,
                    kPpuRenderFlags_NewRenderer);
    setup_status_bg1(ppu, fn, uniform_palette);
    PpuSetExtraSpace(ppu, kExtra);   /* resets layer policies: must come first */
    if (kind == kClampOnly || kind == kClampAndSplit)
        PpuSetWidescreenLayerClampBand(ppu, 0, 0, 225);
    if (kind == kMirrorOnly || kind == kMirrorAndSplit)
        PpuSetWidescreenLayerMirror(ppu, 0x01);
    if (kind == kWorldAndSplit)
        PpuSetWidescreenLayerWorldMirrorBand(ppu, 0, 0, 225, 0, kPpuXPixels);
    if (kind == kIdentityBand) {
        check(PpuSetWidescreenLayerElasticSplitBandSlot(ppu, 0, 0, 0, 225,
                                                        0, 0, 0, 0),
              "identity split band accepted");
    } else if (kind == kSplitBand || kind == kClampAndSplit ||
               kind == kMirrorAndSplit || kind == kWorldAndSplit) {
        check(PpuSetWidescreenLayerElasticSplitBandSlot(ppu, 0, 0, 0, 225,
                                                        kBarL0, kBarL1,
                                                        kBarR0, kBarR1),
              "gauge split band accepted");
    }
    ppu->hScroll[0] = 0;             /* the raster HUD pins hScroll to 0 */
    ppu_runLine(ppu, 0);
    ppu->hScroll[0] = 0;
    ppu_runLine(ppu, kLine);
}

/* Count columns of one gauge that hold the filled colour. `lo`/`hi` bound it
 * in destination space. */
static int count_filled(const uint32_t *buf, int lo, int hi, uint32_t filled) {
    int n = 0;
    for (int x = lo; x < hi; x++)
        if (buf[x + kExtra] == filled)
            n++;
    return n;
}

int main(void) {
    Ppu *ppu = ppu_init();
    if (!ppu) return 2;

    uint32_t *base = malloc(sizeof(uint32_t) * kWide);
    uint32_t *out = malloc(sizeof(uint32_t) * kWide);
    uint32_t *ref = malloc(sizeof(uint32_t) * kWide);
    if (!base || !out || !ref) { fprintf(stderr, "FAIL: oom\n"); return 2; }

    /* The five segments the split builder must produce for GWED's gauge row. */
    const PpuWsElasticSeg expect[5] = {
        { 0,       kBarL0,      -kExtra,           kBarL0 - kExtra },
        { kBarL0,  kBarL1,      kBarL0 - kExtra,   kBarL1 },
        { kBarL1,  kBarR0,      kBarL1,            kBarR0 },
        { kBarR0,  kBarR1,      kBarR0,            kBarR1 + kExtra },
        { kBarR1,  kPpuXPixels, kBarR1 + kExtra,   kPpuXPixels + kExtra },
    };

    render_wide(ppu, base, col_distinct, kNoPolicy);
    render_wide(ppu, out, col_distinct, kSplitBand);

    check(ppu->wsElasticNSeg[0][0] == 5,
          "the split builder emits five segments");
    check(memcmp(ppu->wsElasticSeg[0][0], expect, sizeof expect) == 0,
          "the split builder's segments are the documented layout");
    check(PpuWidescreenLayerElasticBandAt(ppu, 0, kLine) == 0,
          "a configured elastic band is reported for its scanlines");

    /* Sanity: the synthetic line really does label its columns, or every
     * mapping assertion below would be vacuous. Uniqueness is asserted over
     * the widest window a 4bpp layer can express (see col_distinct). */
    {
        int dupes = 0;
        for (int i = 0; i < kPpuXPixels; i++)
            for (int j = i + 1; j < kPpuXPixels && j - i < 64; j++)
                if (base[i + kExtra] == base[j + kExtra])
                    dupes++;
        check(dupes == 0,
              "native columns are uniquely coloured within any 64-px window");
    }

    {
        /* 1. Rigid segments byte-exact at their shifted destination. */
        int rigid_mismatch = 0, rigid_cols = 0;
        for (int i = 0; i < 5; i++) {
            if (expect[i].srcX1 - expect[i].srcX0 !=
                expect[i].dstX1 - expect[i].dstX0)
                continue;   /* elastic */
            for (int x = expect[i].dstX0; x < expect[i].dstX1; x++) {
                int sx = expect[i].srcX0 + (x - expect[i].dstX0);
                rigid_cols++;
                if (out[x + kExtra] != base[sx + kExtra])
                    rigid_mismatch++;
            }
        }
        check(rigid_cols == kBarL0 + (kBarR0 - kBarL1) +
                            (kPpuXPixels - kBarR1),
              "the rigid segments cover the caps and the centre group");
        check(rigid_mismatch == 0,
              "rigid segments are byte-exact at their shifted destination");

        /* 2. The centre group does not move at all. */
        check(memcmp(out + kBarL1 + kExtra, base + kBarL1 + kExtra,
                     sizeof(uint32_t) * (kBarR0 - kBarL1)) == 0,
              "the centre group is byte-identical to the native render");

        /* The anchored groups really did move outward. */
        check(out[0] == base[0 + kExtra],
              "the left frame cap is anchored to the widened left edge");
        check(out[kWide - 1] == base[kPpuXPixels - 1 + kExtra],
              "the right frame cap is anchored to the widened right edge");
        check(out[kExtra] != base[kExtra],
              "native column 0 no longer shows column 0 (the group shifted)");

        /* 3. Every destination column equals the documented mapping, and the
         *    elastic segments preserve their endpoints. */
        int map_mismatch = 0;
        for (int i = 0; i < 5; i++) {
            for (int x = expect[i].dstX0; x < expect[i].dstX1; x++) {
                int sx = map_src(&expect[i], x);
                if (sx < 0 || sx >= kPpuXPixels)
                    continue;
                if (out[x + kExtra] != base[sx + kExtra])
                    map_mismatch++;
            }
            int sw = expect[i].srcX1 - expect[i].srcX0;
            int dw = expect[i].dstX1 - expect[i].dstX0;
            if (sw == dw)
                continue;
            check(out[expect[i].dstX0 + kExtra] ==
                      base[expect[i].srcX0 + kExtra],
                  "an elastic segment starts on its first source column");
            check(out[expect[i].dstX1 - 1 + kExtra] ==
                      base[expect[i].srcX1 - 1 + kExtra],
                  "an elastic segment ends on its last source column");
        }
        check(map_mismatch == 0,
              "every destination column samples the documented source column");

        /* 6. Nothing left transparent: the anchoring opened no gap. */
        int blank = 0;
        for (int x = 0; x < kWide; x++)
            if (out[x] == 0)
                blank++;
        check(blank == 0, "no destination column is left unpainted");
    }

    /* 4. A run of identical source columns stretches to identical output
     *    columns -- the property that makes a plain-chrome bridge invisible. */
    {
        g_filled = kBarLen;              /* both gauges completely full */
        render_wide(ppu, out, col_gauge, kSplitBand);
        int nonuniform = 0;
        for (int x = kBarL0 - kExtra; x < kBarL1 - 1; x++)
            if (out[x + kExtra] != out[x + 1 + kExtra])
                nonuniform++;
        check(nonuniform == 0,
              "a uniform source run stretches to a uniform destination run");
    }

    /* 5. Fill fraction survives the stretch, for every k. */
    {
        g_filled = kBarLen;
        render_wide(ppu, base, col_gauge, kNoPolicy);
        const uint32_t filled_rgb = base[(kBarL1 - 1) + kExtra];
        const uint32_t empty_rgb = base[(kBarL0 - 1) + kExtra];
        check(filled_rgb != empty_rgb, "gauge fill and chrome differ");
        const int dw = kBarLen + kExtra;
        int worst = 0, worst_k = -1, worst_asym = 0;
        for (int k = 0; k <= kBarLen; k++) {
            g_filled = k;
            render_wide(ppu, out, col_gauge, kSplitBand);
            int got = count_filled(out, kBarL0 - kExtra, kBarL1, filled_rgb);
            int got_r = count_filled(out, kBarR0, kBarR1 + kExtra, filled_rgb);
            int want = (k * dw + kBarLen / 2) / kBarLen;  /* round(k*dw/n) */
            int err = got > want ? got - want : want - got;
            if (err > worst) { worst = err; worst_k = k; }
            int asym = got > got_r ? got - got_r : got_r - got;
            if (asym > worst_asym) worst_asym = asym;
            if (k == 0)
                check(got == 0 && got_r == 0, "an empty gauge stays empty");
            if (k == kBarLen)
                check(got == dw && got_r == dw, "a full gauge stays full");
        }
        if (worst > 1)
            fprintf(stderr, "  worst fill error %d px at k=%d\n", worst, worst_k);
        check(worst <= 1, "gauge fill fraction is preserved within 1 px");
        check(worst_asym <= 1,
              "the mirror-image gauge stretches to the same length within 1 px");
    }

    /* 7. Precedence, inertness and rejection. */
    {
        render_wide(ppu, base, col_distinct, kClampOnly);
        render_wide(ppu, out, col_distinct, kIdentityBand);
        check(memcmp(base, out, sizeof(uint32_t) * kWide) == 0,
              "an identity split band renders exactly like a clamp band");

        render_wide(ppu, out, col_distinct, kClampAndSplit);
        check(memcmp(base, out, sizeof(uint32_t) * kWide) == 0,
              "clamp band wins over an overlapping elastic band");

        render_wide(ppu, ref, col_distinct, kMirrorOnly);
        render_wide(ppu, out, col_distinct, kMirrorAndSplit);
        check(memcmp(ref, out, sizeof(uint32_t) * kWide) == 0,
              "layer mirror wins over an overlapping elastic band");

        /* But an elastic band wins over an overlapping WORLD band: it is the
         * more specific statement about this scanline's own layout. */
        render_wide(ppu, ref, col_distinct, kSplitBand);
        render_wide(ppu, out, col_distinct, kWorldAndSplit);
        check(memcmp(ref, out, sizeof(uint32_t) * kWide) == 0,
              "an elastic band wins over an overlapping world band");
    }

    /* Inert at authentic width. */
    {
        uint32_t native[kPpuXPixels], banded[kPpuXPixels];
        for (int pass = 0; pass < 2; pass++) {
            uint32_t *dst = pass ? banded : native;
            memset(dst, 0, sizeof native);
            ppu_reset(ppu);
            PpuBeginDrawing(ppu, (uint8_t *)dst,
                            sizeof(uint32_t) * kPpuXPixels,
                            kPpuRenderFlags_NewRenderer);
            setup_status_bg1(ppu, col_distinct, false);
            if (pass) {
                PpuWsElasticSeg s[2] = {
                    { 0, kBarL1, -kExtra, kBarL1 },
                    { kBarL1, kPpuXPixels, kBarL1, kPpuXPixels + kExtra },
                };
                PpuSetWidescreenLayerElasticBand(ppu, 0, 0, 225, s, 2);
            }
            ppu->hScroll[0] = 0;
            ppu_runLine(ppu, 0);
            ppu->hScroll[0] = 0;
            ppu_runLine(ppu, kLine);
        }
        check(memcmp(native, banded, sizeof native) == 0,
              "an elastic band is inert at authentic 256 width");
    }

    /* Validation: bad input clears the slot rather than half-applying it. */
    {
        PpuWsElasticSeg good[2] = {
            { 0, kBarL1, -kExtra, kBarL1 },
            { kBarL1, kPpuXPixels, kBarL1, kPpuXPixels + kExtra },
        };
        ppu_reset(ppu);
        PpuSetExtraSpace(ppu, kExtra);
        PpuSetWidescreenLayerElasticBand(ppu, 0, 0, 225, good, 2);
        check(PpuWidescreenLayerElasticBandAt(ppu, 0, kLine) == 0,
              "a valid explicit band is installed");

        PpuWsElasticSeg overlap[2] = {
            { 0, kBarL1, -kExtra, kBarL1 },
            { kBarL1, kPpuXPixels, kBarL1 - 4, kPpuXPixels + kExtra },
        };
        PpuSetWidescreenLayerElasticBand(ppu, 0, 0, 225, overlap, 2);
        check(PpuWidescreenLayerElasticBandAt(ppu, 0, kLine) < 0,
              "destination-overlapping segments are rejected outright");

        PpuWsElasticSeg oob[1] = { { 0, kPpuXPixels + 1, 0, kPpuXPixels } };
        PpuSetWidescreenLayerElasticBand(ppu, 0, 0, 225, good, 2);
        PpuSetWidescreenLayerElasticBand(ppu, 0, 0, 225, oob, 1);
        check(PpuWidescreenLayerElasticBandAt(ppu, 0, kLine) < 0,
              "a source range past 256 is rejected outright");

        PpuSetWidescreenLayerElasticBand(ppu, 0, 0, 225, good, 2);
        PpuSetWidescreenLayerElasticBand(ppu, 0, 0, 0, good, 2);
        check(PpuWidescreenLayerElasticBandAt(ppu, 0, kLine) < 0,
              "an empty scanline range disables the slot");

        PpuSetWidescreenLayerElasticBand(ppu, 0, 0, 225, good, 2);
        PpuSetWidescreenLayerElasticBand(ppu, 0, 0, 225, good, 0);
        check(PpuWidescreenLayerElasticBandAt(ppu, 0, kLine) < 0,
              "a zero-segment band disables the slot");

        check(!PpuSetWidescreenLayerElasticSplitBandSlot(ppu, 1, 0, 0, 225,
                                                         120, 8, 136, 248),
              "an inverted left elastic run is refused");
        check(PpuWidescreenLayerElasticBandAt(ppu, 0, kLine) < 0,
              "a refused split band leaves no slot configured");
        check(!PpuSetWidescreenLayerElasticSplitBandSlot(ppu, 1, 0, 0, 225,
                                                         8, 200, 136, 248),
              "overlapping left/right elastic runs are refused");

        /* Slots are independently addressable and per-scanline. */
        check(PpuSetWidescreenLayerElasticSplitBandSlot(ppu, 2, 0, 100, 200,
                                                        kBarL0, kBarL1,
                                                        kBarR0, kBarR1),
              "slot 2 accepts a band");
        check(PpuWidescreenLayerElasticBandAt(ppu, 0, 150) == 2 &&
              PpuWidescreenLayerElasticBandAt(ppu, 0, 50) < 0,
              "slot 2 is independently addressable");
        check(kPpuWsElasticBands >= 5,
              "there are enough slots for GWED's per-tile-row HUD layout");
    }

    free(base);
    free(out);
    free(ref);
    ppu_free(ppu);
    printf("ppu_elastic_band_test: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
