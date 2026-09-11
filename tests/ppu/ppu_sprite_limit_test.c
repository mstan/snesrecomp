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

static void setup_mode2_offset_bg1(Ppu *ppu) {
    ppu->inidisp = 0x0f;
    ppu->bgmode = 2;
    ppu->screenEnabled[1] = 1;
    ppu->bgXsc[2] = 0x04;
    ppu->bgTileAdr = 0x0001;
    ppu->cgwsel = 0x02;
    ppu->cgadsub = 0x60;
    for (size_t i = 0; i < sizeof ppu->vram / sizeof ppu->vram[0]; i++)
        ppu->vram[i] = 0;
    ppu->cgram[0] = 0;
    ppu->cgram[1] = 0x001f;
    ppu->cgram[2] = 0x03e0;

    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 32; x++) {
            ppu->vram[y * 32 + x] = x == 0 ? 0 : 1;
            ppu->vram[0x400 + y * 32 + x] = 0x2010;
        }
    }
    for (int row = 0; row < 8; row++) {
        ppu->vram[0x1000 + row] = 0x00ff;
        ppu->vram[0x1000 + 16 + row] = 0xff00;
    }
}

static void render_one_line(Ppu *ppu) {
    ppu_runLine(ppu, 0);
    ppu_runLine(ppu, 1);
}

static void render_strict_left_oam_frame(Ppu *ppu, uint32_t *pixels,
                                         size_t pixel_count, int raw_x,
                                         const uint8_t *hints) {
    memset(pixels, 0, pixel_count * sizeof(pixels[0]));
    PpuWsSetOamLeftHints(ppu, hints);
    setup_one_sprite_line(ppu, raw_x);
    ppu_runLine(ppu, 0);
    ppu_runLine(ppu, 1);
    snes_frame_counter++;
}

static unsigned count_argb_pixels(const uint32_t *pixels, size_t count) {
    unsigned nonzero = 0;
    for (size_t i = 0; i < count; i++) {
        if (pixels[i] != 0)
            nonzero++;
    }
    return nonzero;
}

int main(void) {
    enum { kPitch = kPpuXPixels * 4 };
    uint8_t pixels[kPitch];
    Ppu *ppu = ppu_init();
    int failures = 0;
    if (!ppu) return 2;
    /* Presentation VRAM changes scanout in both renderers, but never CPU
     * reads/writes, the real tile memory, or the default-off picture. */
    for (unsigned flags = 0; flags <= 1; flags++) {
        uint16_t picture[32768], original[32768];
        uint8_t baseline[kPitch], changed[kPitch], restored[kPitch];
        ppu_reset(ppu);
        setup_mode2_offset_bg1(ppu);
        memcpy(original, ppu->vram, sizeof(original));
        memcpy(picture, original, sizeof(picture));
        for (unsigned row = 0; row < 8; row++) picture[0x1000 + row] = 0xff00;
        PpuBeginDrawing(ppu, baseline, kPitch, flags);
        render_one_line(ppu);
        ppu->renderVram = picture;
        PpuBeginDrawing(ppu, changed, kPitch, flags);
        render_one_line(ppu);
        failures += check(memcmp(baseline, changed, kPitch) != 0,
                          "explicit picture memory changes scanout");
        failures += check(!memcmp(ppu->vram, original, sizeof(original)),
                          "picture rendering preserves real VRAM");
        ppu->vramPointer = 0x1000;
        ppu_write(ppu, 0x16, 0);
        ppu_write(ppu, 0x17, 0x10);
        failures += check(ppu->vramReadBuffer == original[0x1000],
                          "CPU VRAM read buffer ignores picture memory");
        ppu_write(ppu, 0x18, 0x55);
        failures += check((ppu->vram[0x1000] & 255) == 0x55 &&
                          picture[0x1000] == 0xff00,
                          "CPU writes update real memory only");
        memcpy(ppu->vram, original, sizeof(original));
        ppu->renderVram = NULL;
        PpuBeginDrawing(ppu, restored, kPitch, flags);
        render_one_line(ppu);
        failures += check(!memcmp(baseline, restored, kPitch),
                          "unbinding restores the faithful picture");
        ppu->renderVram = picture;
        ppu_reset(ppu);
        failures += check(ppu->renderVram == NULL, "reset discards picture binding");
    }
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
    render_one_line(ppu);
    failures += check((ppu->objBuffer.data[kPpuExtraLeftRight] & 0xff) != 0,
                      "disabling sprite limits renders the low slot");

    /* Mode 2 OPT must still treat x=0 as the hardware left edge when
     * widescreen margins are active. Otherwise backdrop + halved subscreen
     * math samples offset BG columns and replaces authentic columns. */
    {
        enum { kExtra = 8, kWidePixels = kPpuXPixels + kExtra * 2 };
        uint32_t native_pixels[kPpuXPixels] = {0};
        uint32_t wide_pixels[kWidePixels] = {0};
        int mismatches = 0;

        ppu_reset(ppu);
        PpuBeginDrawing(ppu, (uint8_t *)native_pixels,
                        sizeof(uint32_t) * kPpuXPixels,
                        kPpuRenderFlags_NewRenderer);
        setup_mode2_offset_bg1(ppu);
        render_one_line(ppu);

        ppu_reset(ppu);
        PpuBeginDrawing(ppu, (uint8_t *)wide_pixels,
                        sizeof(uint32_t) * kWidePixels,
                        kPpuRenderFlags_NewRenderer);
        PpuSetExtraSpace(ppu, kExtra);
        setup_mode2_offset_bg1(ppu);
        render_one_line(ppu);

        for (int x = 0; x < kPpuXPixels; x++) {
            if (native_pixels[x] !=
                wide_pixels[kExtra + x])
                mismatches++;
        }
        failures += check(mismatches == 0,
                          "Mode 2 half-color subscreen keeps authentic columns with extra space");
        failures += check(wide_pixels[kExtra] != 0,
                          "Mode 2 half-color subscreen draws widened first authentic column");
    }

    /* An armed OBJ overlay captures all OAM slots by default. Hosts may still
     * narrow the slot range explicitly, but the default should not silently
     * export an empty surface. */
    {
        uint32_t frame_pixels[kPpuBufWidth] = {0};
        uint32_t obj_pixels[kPpuXPixels] = {0};

        ppu_reset(ppu);
        PpuBeginDrawing(ppu, (uint8_t *)frame_pixels,
                        sizeof(uint32_t) * kPpuBufWidth,
                        kPpuRenderFlags_NewRenderer);
        failures += check(PpuBindOverlaySurface(
                              ppu, kPpuOverlaySource_Obj,
                              (uint8_t *)obj_pixels,
                              sizeof(uint32_t) * kPpuXPixels),
                          "OBJ overlay surface binds");
        failures += check(PpuSetOverlayCapture(
                              ppu, kPpuOverlaySource_Obj, 0, 0, 8, 1,
                              kPpuOverlayFlag_RemoveFromGame),
                          "OBJ overlay capture arms");
        setup_one_sprite_line(ppu, 0);
        ppu_runLine(ppu, 0);
        ppu_runLine(ppu, 1);
        failures += check(count_argb_pixels(obj_pixels, 8) == 8,
                          "default OBJ capture exports selected pixels");
        failures += check((ppu->objBuffer.data[kPpuExtraLeftRight] & 0xff) == 0,
                          "RemoveFromGame drops captured OBJ from frame");

        memset(frame_pixels, 0, sizeof frame_pixels);
        memset(obj_pixels, 0, sizeof obj_pixels);
        ppu_reset(ppu);
        PpuBeginDrawing(ppu, (uint8_t *)frame_pixels,
                        sizeof(uint32_t) * kPpuBufWidth,
                        kPpuRenderFlags_NewRenderer);
        failures += check(PpuBindOverlaySurface(
                              ppu, kPpuOverlaySource_Obj,
                              (uint8_t *)obj_pixels,
                              sizeof(uint32_t) * kPpuXPixels),
                          "OBJ overlay surface rebinds");
        failures += check(PpuSetOverlayCapture(
                              ppu, kPpuOverlaySource_Obj, 0, 0, 8, 1,
                              kPpuOverlayFlag_RemoveFromGame),
                          "OBJ overlay capture rearms");
        failures += check(PpuSetOverlayOamRange(ppu, 1, 1),
                          "OBJ overlay range narrows capture");
        setup_one_sprite_line(ppu, 0);
        ppu_runLine(ppu, 0);
        ppu_runLine(ppu, 1);
        failures += check(count_argb_pixels(obj_pixels, 8) == 0,
                          "narrowed OBJ capture excludes other slots");
        failures += check((ppu->objBuffer.data[kPpuExtraLeftRight] & 0xff) != 0,
                          "excluded OBJ remains in frame");
        failures += check(PpuBindOverlaySurface(
                              ppu, kPpuOverlaySource_Obj, NULL, 0),
                          "OBJ overlay surface unbinds");
    }

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

    /* Deluxe's BG3 league menu is revealed by overlapping XOR windows.
     * Check all hardware operations against the final rendered pixels. */
    for (unsigned op = 0; op < 4; ++op) {
        ppu_reset(ppu);
        PpuBeginDrawing(ppu, pixels, kPitch, kPpuRenderFlags_NewRenderer);
        ppu->inidisp = 15;
        ppu->bgmode = 9;
        ppu->screenEnabled[0] = ppu->screenWindowed[0] = 4;
        ppu->windowsel = 0x0a00;
        ppu->wbgobjlog = op << 4;
        ppu->window1left = 32; ppu->window1right = 127;
        ppu->window2left = 64; ppu->window2right = 159;
        ppu->bgTileAdr = 0x100;
        for (unsigned i = 0; i < 8; ++i) ppu->vram[0x1000 + i] = 0xff;
        ppu->cgram[1] = 0x7fff;
        ppu_runLine(ppu, 0);
        ppu_runLine(ppu, 1);
        for (unsigned x = 0; x < 256; ++x) {
            bool a = x >= 32 && x <= 127, b = x >= 64 && x <= 159;
            bool masked = op == 0 ? a || b : op == 1 ? a && b :
                          op == 2 ? a != b : a == b;
            failures += check((pixels[x * 4] != 0) == !masked,
                              "BG3 window operation controls visible pixels");
        }
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

    /* Strict left hints only describe host-owned slots. For game-owned slots,
     * a negative-X sprite that is moving frame-to-frame should continue through
     * the widened margin, but a parked off-screen sprite should still clip. */
    {
        enum { kExtra = 16, kWidePixels = kPpuXPixels + kExtra * 2 };
        uint32_t wide_pixels[kWidePixels];
        uint8_t no_hints[16] = {0};

        ppu_reset(ppu);
        snes_frame_counter = 1000;
        PpuBeginDrawing(ppu, (uint8_t *)wide_pixels,
                        sizeof(uint32_t) * kWidePixels,
                        kPpuRenderFlags_NewRenderer);
        PpuSetExtraSpace(ppu, kExtra);

        render_strict_left_oam_frame(ppu, wide_pixels, kWidePixels,
                                     0x1f9, no_hints);
        failures += check(wide_pixels[kExtra] != 0,
                          "partially visible negative OAM stays visible");

        render_strict_left_oam_frame(ppu, wide_pixels, kWidePixels,
                                     0x1f8, no_hints);
        failures += check(wide_pixels[kExtra - 1] != 0 &&
                              wide_pixels[kExtra - 8] != 0,
                          "moving unhinted OAM exits through left margin");

        for (int i = 0; i < kPpuWsOamMovingGraceFrames; i++) {
            render_strict_left_oam_frame(ppu, wide_pixels, kWidePixels,
                                         0x1f8, no_hints);
        }
        failures += check(wide_pixels[kExtra - 1] == 0 &&
                              wide_pixels[kExtra - 8] == 0,
                          "stopped unhinted OAM parks outside left edge");

        ppu_reset(ppu);
        snes_frame_counter = 2000;
        PpuBeginDrawing(ppu, (uint8_t *)wide_pixels,
                        sizeof(uint32_t) * kWidePixels,
                        kPpuRenderFlags_NewRenderer);
        PpuSetExtraSpace(ppu, kExtra);
        render_strict_left_oam_frame(ppu, wide_pixels, kWidePixels,
                                     0x1f8, no_hints);
        failures += check(wide_pixels[kExtra - 1] == 0 &&
                              wide_pixels[kExtra - 8] == 0,
                          "initial parked unhinted OAM remains clipped");
    }

    ppu_free(ppu);
    if (failures) return 1;
    puts("ppu_sprite_limit_test: PASS");
    return 0;
}
