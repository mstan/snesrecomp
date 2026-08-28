/* Synthetic regression for SNES OBJ range/fetch ordering.
 * No game ROM, generated data, or platform frontend is required. */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "snes/ppu.h"
#include "snes/snes.h"

Snes *g_snes;

/* ppu.c references these runtime globals, but this harness links ppu.c alone. */
int snes_frame_counter;
unsigned char g_snesrecomp_last_hdmaen;

uint16_t WsShadowTile(int layer, int screen_x, uint32_t wrapped_y,
                      uint16_t real_tile) {
    (void)layer;
    (void)screen_x;
    (void)wrapped_y;
    return real_tile;
}

bool WsShadowLayerActive(int layer) {
    (void)layer;
    return false;
}

uint32_t WsShadowWorldX(int layer) {
    (void)layer;
    return 0;
}

uint32_t WsShadowPresentWorldY(int layer, int screen_x) {
    (void)layer;
    (void)screen_x;
    return 0;
}

uint32_t WsShadowScrollY(int layer) {
    (void)layer;
    return 0;
}

void WsShadowOnVramWrite(uint16_t word_adr, uint16_t value) {
    (void)word_adr;
    (void)value;
}

static int check(bool condition, const char *message) {
    if (!condition) fprintf(stderr, "FAIL: %s\n", message);
    return condition ? 0 : 1;
}

static void no_op_line_enhancer(Ppu *ppu, uint y, bool sub, void *context) {
    (void)ppu;
    (void)y;
    (void)sub;
    (void)context;
}

static void setup_solid_obj(Ppu *ppu, int slot, int x, int y) {
    int high_index = slot >> 2;
    int x_high_bit = (slot & 3) * 2;
    ppu->oam[slot * 2] = (uint16_t)((y << 8) | (x & 0xff));
    ppu->oam[slot * 2 + 1] = 0;
    if (x & 0x100)
        ppu->highOam[high_index] |= (uint8_t)(1u << x_high_bit);
    else
        ppu->highOam[high_index] &= (uint8_t)~(1u << x_high_bit);
}

static void setup_one_sprite_line(Ppu *ppu, int raw_x) {
    ppu->inidisp = 0x0f;
    ppu->screenEnabled[0] = 1 << 4;
    memset(ppu->highOam, 0, sizeof ppu->highOam);
    for (int slot = 0; slot < 128; slot++)
        ppu->oam[slot * 2] = 0xf000;
    setup_solid_obj(ppu, 0, raw_x, 0);
    for (size_t i = 0; i < sizeof ppu->vram / sizeof ppu->vram[0]; i++)
        ppu->vram[i] = 0xffff;
    ppu->cgram[0] = 0;
    for (size_t i = 1; i < sizeof ppu->cgram / sizeof ppu->cgram[0]; i++)
        ppu->cgram[i] = 0x7fff;
}

int main(void) {
    enum { kPitch = kPpuXPixels * 4 };
    uint8_t pixels[kPitch];
    Ppu *ppu = ppu_init();
    int failures = 0;
    if (!ppu) return 2;
    memset(pixels, 0, sizeof pixels);
    ppu_reset(ppu);
    PpuBeginDrawing(ppu, pixels, kPitch, kPpuRenderFlags_NewRenderer);
    ppu->inidisp = 0x0f;

    /* Keep unused OAM entries off this line. Slot 0 is one 8x8 sprite at x=0;
     * slots 1..5 are 64x64 sprites at x=64. Reverse tile fetch reaches the
     * 34-sliver limit before slot 0, while a forward one-pass implementation
     * incorrectly renders it. */
    for (int slot = 0; slot < 128; slot++)
        ppu->oam[slot * 2] = 0xf000;
    ppu->obsel = 2 << 5;  /* size pair 8x8 / 64x64 */
    ppu->oam[0] = 0x0000;
    for (int slot = 1; slot <= 5; slot++) {
        ppu->oam[slot * 2] = 0x0040;
        int high_byte = slot >> 2;
        int size_bit = ((slot & 3) * 2) + 1;
        ppu->highOam[high_byte] |= (uint8_t)(1u << size_bit);
    }
    for (size_t i = 0; i < sizeof ppu->vram / sizeof ppu->vram[0]; i++)
        ppu->vram[i] = 0xffff;

    ppu_runLine(ppu, 0);
    ppu_runLine(ppu, 1);
    failures += check(ppu->timeOver, "34-sliver overflow is reported");
    failures += check((ppu->objBuffer.data[kPpuExtraLeftRight] & 0xff) == 0,
                      "reverse fetch drops low slot after sliver overflow");

    PpuBeginDrawing(ppu, pixels, kPitch,
                    kPpuRenderFlags_NewRenderer |
                    kPpuRenderFlags_NoSpriteLimits);
    ppu_runLine(ppu, 0);
    ppu_runLine(ppu, 1);
    failures += check((ppu->objBuffer.data[kPpuExtraLeftRight] & 0xff) != 0,
                      "disabling sprite limits renders the low slot");

    /* Existing line-enhancer users rely on BG1 staying inside its authentic
     * destination viewport. New title-specific source insets must be opt-in
     * and must not relax that legacy fallback. */
    {
        enum { kExtra = 8, kWidePixels = kPpuXPixels + kExtra * 2 };
        uint32_t wide_pixels[kWidePixels];

        ppu_reset(ppu);
        memset(wide_pixels, 0, sizeof wide_pixels);
        PpuBeginDrawing(ppu, (uint8_t *)wide_pixels,
                        sizeof(uint32_t) * kWidePixels,
                        kPpuRenderFlags_NewRenderer);
        PpuSetExtraSpace(ppu, kExtra);
        PpuSetWidescreenLineEnhancer(ppu, no_op_line_enhancer, NULL);
        ppu->inidisp = 0x0f;
        ppu->bgmode = 1;
        ppu->screenEnabled[0] = 1;
        for (size_t i = 0; i < sizeof ppu->vram / sizeof ppu->vram[0]; i++)
            ppu->vram[i] = 0xffff;
        ppu->cgram[0] = 0;
        for (size_t i = 1; i < sizeof ppu->cgram / sizeof ppu->cgram[0]; i++)
            ppu->cgram[i] = 0x7fff;

        ppu_runLine(ppu, 0);
        ppu_runLine(ppu, 1);
        failures += check(wide_pixels[0] == 0 &&
                              wide_pixels[kExtra - 1] == 0 &&
                              wide_pixels[kExtra + kPpuXPixels] == 0,
                          "legacy line enhancer keeps BG1 out of margins");
        failures += check(wide_pixels[kExtra] != 0 &&
                              wide_pixels[kExtra + kPpuXPixels - 1] != 0,
                          "legacy line enhancer retains native BG1");

        memset(wide_pixels, 0, sizeof wide_pixels);
        PpuSetExtraSpace(ppu, kExtra);
        PpuSetWidescreenLayerViewportInset(ppu, 0, 16, 16);
        ppu_runLine(ppu, 0);
        ppu_runLine(ppu, 1);
        failures += check(wide_pixels[kExtra] == 0 &&
                              wide_pixels[kExtra + 15] == 0 &&
                              wide_pixels[kExtra + 240] == 0,
                          "explicit BG1 viewport inset hides native padding");
        failures += check(wide_pixels[kExtra + 16] != 0 &&
                              wide_pixels[kExtra + 239] != 0,
                          "explicit BG1 viewport inset retains visible span");
    }

    /* The parity renderer is also SMK's widescreen Mode 7 path. Live margins
     * must sample real map coordinates, while centered extra space remains
     * an exact native-width presentation for menus. */
    {
        enum { kExtra = 8, kWidePixels = kPpuXPixels + kExtra * 2 };
        uint32_t wide_pixels[kWidePixels];

        ppu_reset(ppu);
        memset(wide_pixels, 0, sizeof wide_pixels);
        PpuBeginDrawing(ppu, (uint8_t *)wide_pixels,
                        sizeof(uint32_t) * kWidePixels, 0);
        PpuSetExtraSpace(ppu, kExtra);
        ppu->inidisp = 0x0f;
        ppu->bgmode = 7;
        ppu->screenEnabled[0] = 1;
        ppu->m7matrix[0] = 0x0100;
        ppu->m7matrix[3] = 0x0100;
        for (size_t i = 0; i < sizeof ppu->vram / sizeof ppu->vram[0]; i++)
            ppu->vram[i] = 0x0101;
        ppu->cgram[1] = 0x7fff;

        ppu_runLine(ppu, 0);
        ppu_runLine(ppu, 1);
        failures += check(wide_pixels[0] != 0 &&
                              wide_pixels[kExtra - 1] != 0 &&
                              wide_pixels[kExtra + kPpuXPixels] != 0 &&
                              wide_pixels[kWidePixels - 1] != 0,
                          "legacy Mode 7 renders live side margins");

        memset(wide_pixels, 0, sizeof wide_pixels);
        PpuSetExtraSpaceCentered(ppu, kExtra);
        ppu_runLine(ppu, 0);
        ppu_runLine(ppu, 1);
        failures += check(wide_pixels[0] == 0 &&
                              wide_pixels[kExtra - 1] == 0 &&
                              wide_pixels[kExtra + kPpuXPixels] == 0 &&
                              wide_pixels[kWidePixels - 1] == 0,
                          "legacy Mode 7 preserves centered margins");
        failures += check(wide_pixels[kExtra] != 0 &&
                              wide_pixels[kExtra + kPpuXPixels - 1] != 0,
                          "legacy Mode 7 retains centered native columns");
    }

    /* Legacy-renderer titles use the same opt-in HUD anchor bands as the
     * current renderer. The inserted spans are transparent, while the source
     * pixels on either side retain their distance from the window edges. */
    {
        enum {
            kExtra = 8,
            kLeftEnd = 32,
            kRightStart = 224,
            kWidePixels = kPpuXPixels + kExtra * 2
        };
        uint32_t wide_pixels[kWidePixels];

        ppu_reset(ppu);
        memset(wide_pixels, 0, sizeof wide_pixels);
        PpuBeginDrawing(ppu, (uint8_t *)wide_pixels,
                        sizeof(uint32_t) * kWidePixels, 0);
        PpuSetExtraSpace(ppu, kExtra);
        PpuSetWidescreenLayerAnchorBandSlot(
            ppu, 1, 1, 0, 2, kLeftEnd, kRightStart);
        ppu->inidisp = 0x0f;
        ppu->bgmode = 1;
        ppu->screenEnabled[0] = 1 << 1;
        for (size_t i = 0; i < sizeof ppu->vram / sizeof ppu->vram[0]; i++)
            ppu->vram[i] = 0xffff;
        ppu->cgram[0] = 0;
        for (size_t i = 1; i < sizeof ppu->cgram / sizeof ppu->cgram[0]; i++)
            ppu->cgram[i] = 0x7fff;

        ppu_runLine(ppu, 0);
        ppu_runLine(ppu, 1);
        failures += check(
            wide_pixels[0] != 0 &&
                wide_pixels[kLeftEnd - 1] != 0 &&
                wide_pixels[kExtra + kLeftEnd] != 0 &&
                wide_pixels[kExtra + kRightStart - 1] != 0 &&
                wide_pixels[kExtra * 2 + kRightStart] != 0 &&
                wide_pixels[kWidePixels - 1] != 0,
            "legacy HUD anchors preserve left, center, and right spans");
        failures += check(
            wide_pixels[kLeftEnd] == 0 &&
                wide_pixels[kExtra + kLeftEnd - 1] == 0 &&
                wide_pixels[kExtra + kRightStart] == 0 &&
                wide_pixels[kExtra * 2 + kRightStart - 1] == 0,
            "legacy HUD anchors leave inserted margin spans transparent");
    }

    /* A room boundary may deliberately leave the live world margins empty,
     * while a title-specific HUD still uses the full centering budget. Verify
     * both supported HUD sources: an anchored 4bpp background and explicitly
     * selected OAM slots. Unselected world sprites must remain centered. */
    {
        enum { kExtra = 8, kWidePixels = kPpuXPixels + kExtra * 2 };
        uint32_t wide_pixels[kWidePixels];

        ppu_reset(ppu);
        memset(wide_pixels, 0, sizeof wide_pixels);
        PpuBeginDrawing(ppu, (uint8_t *)wide_pixels,
                        sizeof(uint32_t) * kWidePixels,
                        kPpuRenderFlags_NewRenderer);
        PpuSetExtraSpaceCentered(ppu, kExtra);
        PpuSetWidescreenLayerAnchorBand(ppu, 1, 0, 2, 32, 224);
        PpuSetWidescreenHudAlwaysVisible(ppu, true);
        ppu->inidisp = 0x0f;
        ppu->bgmode = 1;
        ppu->screenEnabled[0] = 1 << 1;
        for (size_t i = 0; i < sizeof ppu->vram / sizeof ppu->vram[0]; i++)
            ppu->vram[i] = 0xffff;
        ppu->cgram[0] = 0;
        for (size_t i = 1; i < sizeof ppu->cgram / sizeof ppu->cgram[0]; i++)
            ppu->cgram[i] = 0x7fff;

        ppu_runLine(ppu, 0);
        ppu_runLine(ppu, 1);
        failures += check(wide_pixels[0] != 0 &&
                              wide_pixels[kWidePixels - 1] != 0,
                          "always-visible BG HUD uses full centered margins");

        ppu_reset(ppu);
        memset(wide_pixels, 0, sizeof wide_pixels);
        PpuBeginDrawing(ppu, (uint8_t *)wide_pixels,
                        sizeof(uint32_t) * kWidePixels,
                        kPpuRenderFlags_NewRenderer);
        PpuSetExtraSpaceCentered(ppu, kExtra);
        PpuSetWsHudOamBand(ppu, 2, 112, 160);
        PpuSetWsHudOamShiftRange(ppu, 0, 1);
        PpuSetWidescreenHudAlwaysVisible(ppu, true);
        ppu->inidisp = 0x0f;
        ppu->screenEnabled[0] = 1 << 4;
        for (int slot = 0; slot < 128; slot++)
            ppu->oam[slot * 2] = 0xf000;
        ppu->oam[0] = 0x0000;
        for (size_t i = 0; i < sizeof ppu->vram / sizeof ppu->vram[0]; i++)
            ppu->vram[i] = 0xffff;
        ppu->cgram[0] = 0;
        for (size_t i = 1; i < sizeof ppu->cgram / sizeof ppu->cgram[0]; i++)
            ppu->cgram[i] = 0x7fff;

        ppu_runLine(ppu, 0);
        ppu_runLine(ppu, 1);
        failures += check(wide_pixels[0] != 0 &&
                              wide_pixels[kExtra] == 0,
                          "selected HUD OAM shifts into centered margin");

        ppu_reset(ppu);
        memset(wide_pixels, 0, sizeof wide_pixels);
        PpuBeginDrawing(ppu, (uint8_t *)wide_pixels,
                        sizeof(uint32_t) * kWidePixels,
                        kPpuRenderFlags_NewRenderer);
        PpuSetExtraSpaceCentered(ppu, kExtra);
        PpuSetWsHudOamBand(ppu, 2, 112, 160);
        PpuSetWsHudOamShiftRange(ppu, 1, 1);
        PpuSetWidescreenHudAlwaysVisible(ppu, true);
        ppu->inidisp = 0x0f;
        ppu->screenEnabled[0] = 1 << 4;
        for (int slot = 0; slot < 128; slot++)
            ppu->oam[slot * 2] = 0xf000;
        ppu->oam[0] = 0x0000;
        for (size_t i = 0; i < sizeof ppu->vram / sizeof ppu->vram[0]; i++)
            ppu->vram[i] = 0xffff;
        ppu->cgram[0] = 0;
        for (size_t i = 1; i < sizeof ppu->cgram / sizeof ppu->cgram[0]; i++)
            ppu->cgram[i] = 0x7fff;

        ppu_runLine(ppu, 0);
        ppu_runLine(ppu, 1);
        failures += check(wide_pixels[0] == 0 &&
                              wide_pixels[kExtra] != 0,
                          "unselected world OAM remains centered");
    }

    /* Strict OAM margin hints are symmetric. A raw negative X may be a parked
     * hardware-hidden sprite or a genuine widened-world sprite. Hosts can mark
     * the genuine slots; unmarked fully-offscreen-left slots stay clipped. */
    {
        enum { kExtra = 16, kWidePixels = kPpuXPixels + kExtra * 2 };
        uint32_t wide_pixels[kWidePixels];
        uint8_t no_hints[16] = {0};
        uint8_t slot0_hint[16] = {1};

        ppu_reset(ppu);
        memset(wide_pixels, 0, sizeof wide_pixels);
        PpuBeginDrawing(ppu, (uint8_t *)wide_pixels,
                        sizeof(uint32_t) * kWidePixels,
                        kPpuRenderFlags_NewRenderer);
        PpuSetExtraSpace(ppu, kExtra);
        PpuWsSetOamLeftHints(ppu, no_hints);
        setup_one_sprite_line(ppu, 0x1f8);
        ppu_runLine(ppu, 0);
        ppu_runLine(ppu, 1);
        failures += check(wide_pixels[kExtra - 1] == 0 &&
                              wide_pixels[kExtra - 8] == 0,
                          "unhinted fully-off-left OAM stays clipped");

        ppu_reset(ppu);
        memset(wide_pixels, 0, sizeof wide_pixels);
        PpuBeginDrawing(ppu, (uint8_t *)wide_pixels,
                        sizeof(uint32_t) * kWidePixels,
                        kPpuRenderFlags_NewRenderer);
        PpuSetExtraSpace(ppu, kExtra);
        PpuWsSetOamLeftHints(ppu, slot0_hint);
        setup_one_sprite_line(ppu, 0x1f8);
        ppu_runLine(ppu, 0);
        ppu_runLine(ppu, 1);
        failures += check(wide_pixels[kExtra - 1] != 0 &&
                              wide_pixels[kExtra - 8] != 0,
                          "hinted fully-off-left OAM renders in left margin");

        ppu_reset(ppu);
        memset(wide_pixels, 0, sizeof wide_pixels);
        PpuBeginDrawing(ppu, (uint8_t *)wide_pixels,
                        sizeof(uint32_t) * kWidePixels,
                        kPpuRenderFlags_NewRenderer);
        PpuSetExtraSpace(ppu, kExtra);
        PpuWsSetOamLeftHints(ppu, no_hints);
        setup_one_sprite_line(ppu, 0x1fc);
        ppu_runLine(ppu, 0);
        ppu_runLine(ppu, 1);
        failures += check(wide_pixels[kExtra] != 0 &&
                              wide_pixels[kExtra + 3] != 0,
                          "strict left hints keep native-edge OAM visible");

        ppu_reset(ppu);
        memset(wide_pixels, 0, sizeof wide_pixels);
        PpuBeginDrawing(ppu, (uint8_t *)wide_pixels,
                        sizeof(uint32_t) * kWidePixels,
                        kPpuRenderFlags_NewRenderer);
        PpuSetExtraSpace(ppu, kExtra);
        PpuWsSetOamRightHints(ppu, no_hints);
        setup_one_sprite_line(ppu, 0x108);
        ppu_runLine(ppu, 0);
        ppu_runLine(ppu, 1);
        failures += check(wide_pixels[kExtra + kPpuXPixels + 8] == 0,
                          "unhinted right-band OAM wraps negative");

        ppu_reset(ppu);
        memset(wide_pixels, 0, sizeof wide_pixels);
        PpuBeginDrawing(ppu, (uint8_t *)wide_pixels,
                        sizeof(uint32_t) * kWidePixels,
                        kPpuRenderFlags_NewRenderer);
        PpuSetExtraSpace(ppu, kExtra);
        PpuWsSetOamRightHints(ppu, slot0_hint);
        setup_one_sprite_line(ppu, 0x108);
        ppu_runLine(ppu, 0);
        ppu_runLine(ppu, 1);
        failures += check(wide_pixels[kExtra + kPpuXPixels + 8] != 0,
                          "hinted right-band OAM remains positive");

        ppu_reset(ppu);
        memset(wide_pixels, 0, sizeof wide_pixels);
        PpuBeginDrawing(ppu, (uint8_t *)wide_pixels,
                        sizeof(uint32_t) * kWidePixels,
                        kPpuRenderFlags_NewRenderer);
        PpuSetExtraSpace(ppu, kExtra);
        PpuWsSetOamLeftHints(ppu, NULL);
        setup_one_sprite_line(ppu, 0x1f8);
        ppu_runLine(ppu, 0);
        ppu_runLine(ppu, 1);
        failures += check(wide_pixels[kExtra - 1] != 0,
                          "NULL left hints restore legacy margin behavior");
    }

    ppu_free(ppu);
    if (failures) return 1;
    puts("ppu_sprite_limit_test: PASS");
    return 0;
}
