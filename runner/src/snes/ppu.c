#include "ppu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "snes.h"
#include "../debug_server.h"
#include "snes_regs.h"
#include "ws_shadow.h"


extern bool g_new_ppu;
extern Snes *g_snes;
void PpuDrawWholeLineOldPpu(Ppu *ppu, int line);
static void PpuDrawWholeLine(Ppu *ppu, uint y);

static bool ppu_evaluateSprites(Ppu* ppu, int line);
static uint16_t ppu_getVramRemap(Ppu* ppu);


Ppu* ppu_init(void) {
  Ppu* ppu = calloc(1, sizeof(Ppu));  /* zero padding: saveload/co-sim hash determinism */
  return ppu;
}

void ppu_free(Ppu* ppu) {
  free(ppu);
}

void ppu_reset(Ppu* ppu) {
  {
    size_t pitch = ppu->renderPitch;
    uint8_t *renderBuffer = ppu->renderBuffer;
    uint32_t renderFlags = ppu->renderFlags;
    uint32_t overlayPitch[kPpuOverlaySource_Count];
    uint8_t *overlayBuffer[kPpuOverlaySource_Count];
    memcpy(overlayPitch, ppu->overlayRenderPitch, sizeof(overlayPitch));
    memcpy(overlayBuffer, ppu->overlayRenderBuffer, sizeof(overlayBuffer));
    memset(ppu, 0, sizeof(*ppu));
    ppu->renderBuffer = renderBuffer;
    ppu->renderPitch = (uint32_t)pitch;
    ppu->renderFlags = renderFlags;
    memcpy(ppu->overlayRenderPitch, overlayPitch, sizeof(overlayPitch));
    memcpy(ppu->overlayRenderBuffer, overlayBuffer, sizeof(overlayBuffer));
  }
  ppu->vramIncrement = 1;
}

void ppu_saveload(Ppu *ppu, SaveLoadInfo *sli) {
  assert(offsetof(Ppu, cgwsel) + 1 - offsetof(Ppu, inidisp) == PPU_SAVESTATE_REGS_SIZE);
  assert(offsetof(Ppu, vram) + 0x10000 - offsetof(Ppu, cgram) == PPU_SAVESTATE_MEM_SIZE);
  uint32 version[2] = {'P' | 'P' << 8 | 'U' << 16 | '0' << 24, PPU_SAVESTATE_REGS_SIZE + PPU_SAVESTATE_MEM_SIZE};
  sli->func(sli, version, 8);
  sli->func(sli, &ppu->inidisp, PPU_SAVESTATE_REGS_SIZE);
  sli->func(sli, &ppu->cgram, PPU_SAVESTATE_MEM_SIZE);
}

// Debug layer isolation: SNESRECOMP_LAYER_MASK is a bitmask of layers to keep
// (bit0=BG1 .. bit3=BG4, bit4=OBJ). Host-only render filter — never serialized,
// never affects guest state. Unset/0xff = all layers (normal).
uint8_t g_snes_ppu_dbg_layer_mask = 0xff;

void PpuBeginDrawing(Ppu *ppu, uint8_t *pixels, size_t pitch, uint32_t render_flags) {
  static bool mask_init;
  if (!mask_init) {
    mask_init = true;
    const char *m = getenv("SNESRECOMP_LAYER_MASK");
    if (m && *m)
      g_snes_ppu_dbg_layer_mask = (uint8_t)strtoul(m, NULL, 0);
  }
  ppu->renderPitch = (uint)pitch;
  ppu->renderBuffer = pixels;
  ppu->renderFlags = render_flags;
}

void PpuSetWidescreenLineEnhancer(Ppu *ppu,
                                  PpuWidescreenLineEnhancer *enhancer,
                                  void *context) {
  ppu->widescreenLineEnhancer = enhancer;
  ppu->widescreenLineEnhancerContext = enhancer ? context : NULL;
}

void PpuClearOverlayCaptures(Ppu *ppu) {
  memset(ppu->overlayCaptures, 0, sizeof(ppu->overlayCaptures));
}

void PpuClearOverlayBindings(Ppu *ppu) {
  memset(ppu->overlayRenderBuffer, 0, sizeof(ppu->overlayRenderBuffer));
  memset(ppu->overlayRenderPitch, 0, sizeof(ppu->overlayRenderPitch));
  PpuClearOverlayCaptures(ppu);
}

bool PpuBindOverlaySurface(Ppu *ppu, PpuOverlaySource source,
                           uint8_t *pixels, size_t pitch) {
  if ((unsigned)source >= kPpuOverlaySource_Count ||
      (pixels && (!pitch || pitch % sizeof(uint32_t) != 0 ||
                  pitch / sizeof(uint32_t) < kPpuXPixels ||
                  pitch / sizeof(uint32_t) > kPpuBufWidth)))
    return false;
  ppu->overlayRenderBuffer[source] = pixels;
  ppu->overlayRenderPitch[source] = pixels ? (uint32_t)pitch : 0;
  if (!pixels)
    memset(&ppu->overlayCaptures[source], 0,
           sizeof(ppu->overlayCaptures[source]));
  return true;
}

bool PpuSetOverlayCapture(Ppu *ppu, PpuOverlaySource source,
                          int x, int y, int width, int height, uint8_t flags) {
  if ((unsigned)source >= kPpuOverlaySource_Count || width <= 0 || height <= 0)
    return false;
  int64_t requested_x1 = (int64_t)x + width;
  int64_t requested_y1 = (int64_t)y + height;
  int x0 = IntMax(x, -kPpuExtraLeftRight);
  int x1 = requested_x1 < kPpuXPixels + kPpuExtraLeftRight
      ? (int)requested_x1 : kPpuXPixels + kPpuExtraLeftRight;
  int y0 = IntMax(y, 0);
  int y1 = requested_y1 < 240 ? (int)requested_y1 : 240;
  if (x1 <= x0 || y1 <= y0)
    return false;
  PpuOverlayCapture *capture = &ppu->overlayCaptures[source];
  capture->x0 = (int16_t)x0;
  capture->x1 = (int16_t)x1;
  capture->y0 = (int16_t)y0;
  capture->y1 = (int16_t)y1;
  capture->flags = flags & kPpuOverlayFlag_RemoveFromGame;
  capture->oamFirst = 0;
  capture->oamCount = 0;
  return true;
}

bool PpuSetOverlayOamRange(Ppu *ppu, uint8_t first, uint8_t count) {
  PpuOverlayCapture *capture =
      &ppu->overlayCaptures[kPpuOverlaySource_Obj];
  if (first >= 128 || !count || count > 128 - first ||
      capture->x1 <= capture->x0 || capture->y1 <= capture->y0)
    return false;
  capture->oamFirst = first;
  capture->oamCount = count;
  return true;
}

static inline void PpuResetLayerPolicies(Ppu *ppu) {
  ppu->wsLayerClamp = 0;
  ppu->wsLayerMirror = 0;
  ppu->wsLayerRepeat = 0;
  memset(ppu->wsClampY0, 0, sizeof(ppu->wsClampY0));
  memset(ppu->wsClampY1, 0, sizeof(ppu->wsClampY1));
  memset(ppu->wsRepeatY0, 0, sizeof(ppu->wsRepeatY0));
  memset(ppu->wsRepeatY1, 0, sizeof(ppu->wsRepeatY1));
  memset(ppu->wsStretchY0, 0, sizeof(ppu->wsStretchY0));
  memset(ppu->wsStretchY1, 0, sizeof(ppu->wsStretchY1));
  memset(ppu->wsMarginGapL, 0, sizeof(ppu->wsMarginGapL));
  memset(ppu->wsMarginGapR, 0, sizeof(ppu->wsMarginGapR));
}

void PpuSetExtraSpace(Ppu *ppu, uint8_t extra) {
  if (extra > kPpuExtraLeftRight)
    extra = kPpuExtraLeftRight;
  // Symmetric border: equal columns added on each side. extraLeftRight is the
  // centering budget; extraLeftCur/extraRightCur are the per-side columns the
  // window/sprite/composite paths actually render.
  ppu->extraLeftRight = extra;
  ppu->extraLeftCur = extra;
  ppu->extraRightCur = extra;
  PpuResetLayerPolicies(ppu);
}

void PpuSetExtraSpaceCentered(Ppu *ppu, uint8_t budget) {
  if (budget > kPpuExtraLeftRight)
    budget = kPpuExtraLeftRight;
  // Render only the authentic 256 columns but keep the centering budget so the
  // composite places them in the middle of the wider framebuffer (the caller
  // clears the side margins to black -> letterbox/pillarbox). Used for bounded
  // screens (overworld, title) where there is no valid BG past 256 to show.
  ppu->extraLeftRight = budget;
  ppu->extraLeftCur = 0;
  ppu->extraRightCur = 0;
  PpuResetLayerPolicies(ppu);
}

void PpuSetExtraSideSpace(Ppu *ppu, int left, int right, int bottom) {
  // Per-frame asymmetric fill within the centering budget (extraLeftRight).
  // Mirrors zelda3's PpuSetExtraSideSpace; left/right clamp to the budget so
  // the line renderer's window edges stay inside the priority buffers, bottom
  // clamps to the 16px overscan band. See ppu.h for the symmetric-vs-dynamic
  // distinction.
  ppu->extraLeftCur = (uint8_t)IntMin(IntMax(left, 0), ppu->extraLeftRight);
  ppu->extraRightCur = (uint8_t)IntMin(IntMax(right, 0), ppu->extraLeftRight);
  ppu->extraBottomCur = (uint8_t)IntMin(IntMax(bottom, 0), 16);
}

void PpuSetWidescreenHudSplit(Ppu *ppu, uint8_t height, uint8_t left_end,
                              uint8_t right_start) {
  // See ppu.h. Chunk bounds must be ordered for the span construction in
  // PpuDrawBackground_2bpp (strictly ascending edges); disable otherwise.
  if (left_end == 0 || left_end >= right_start) height = 0;
  ppu->wsHudSplitHeight = height;
  ppu->wsHudLeftEnd = left_end;
  ppu->wsHudRightStart = right_start;
}

void PpuSetWsHudOamShift(Ppu *ppu, uint8_t nslots) {
  PpuSetWsHudOamShiftRange(ppu, 0, nslots);
}

void PpuSetWsHudOamShiftRange(Ppu *ppu, uint8_t first_slot, uint8_t nslots) {
  if (first_slot >= 128 || nslots == 0) {
    ppu->wsHudOamFirstSlot = 0;
    ppu->wsHudOamSlots = 0;
    return;
  }
  ppu->wsHudOamFirstSlot = first_slot;
  ppu->wsHudOamSlots =
      nslots > 128 - first_slot ? (uint8_t)(128 - first_slot) : nslots;
}

void PpuSetWidescreenBg3Widen(Ppu *ppu, uint8_t from_y) {
  ppu->wsBg3WidenY = from_y;
}

void PpuSetWidescreenLayerMask(Ppu *ppu, uint8_t bg_layer_mask) {
  ppu->wsLayerWidenMask = bg_layer_mask & 0x0f;
}

void PpuSetMode2LayerCapture(Ppu *ppu, int layer) {
  ppu->wsMode2CaptureLayer = layer >= 0 && layer < 2 ? (uint8_t)(layer + 1) : 0;
}

const uint8_t *PpuGetMode2LayerCapture(const Ppu *ppu) {
  return ppu->wsMode2CaptureLayer ? &ppu->wsMode2Capture[0][0] : NULL;
}

const uint8_t *PpuGetMode2Bg1Palette(const Ppu *ppu) {
  return &ppu->wsMode2Bg1Palette[0][0];
}

void PpuSetWidescreenLayerClamp(Ppu *ppu, uint8_t mask) {
  ppu->wsLayerClamp = mask;
}

void PpuSetWidescreenLayerMirror(Ppu *ppu, uint8_t mask) {
  ppu->wsLayerMirror = mask;
}

void PpuSetWidescreenLayerRepeat(Ppu *ppu, uint8_t mask) {
  ppu->wsLayerRepeat = mask;
}

void PpuSetWidescreenLayerClampBand(Ppu *ppu, uint8_t layer, uint8_t y0,
                                    uint8_t y1) {
  if (layer < 4) {
    ppu->wsClampY0[layer] = y0;
    ppu->wsClampY1[layer] = y1;
  }
}

void PpuSetWidescreenLayerRepeatBand(Ppu *ppu, uint8_t layer, uint8_t y0,
                                     uint8_t y1) {
  if (layer < 4) {
    ppu->wsRepeatY0[layer] = y0;
    ppu->wsRepeatY1[layer] = y1;
  }
}

void PpuSetWidescreenLayerStretchBand(Ppu *ppu, uint8_t layer, uint8_t y0,
                                      uint8_t y1) {
  if (layer < 4) {
    ppu->wsStretchY0[layer] = y0;
    ppu->wsStretchY1[layer] = y1;
  }
}

void PpuSetWidescreenLayerMarginGap(Ppu *ppu, uint8_t layer, uint8_t left_px,
                                    uint8_t right_px) {
  if (layer < 4) {
    ppu->wsMarginGapL[layer] = left_px;
    ppu->wsMarginGapR[layer] = right_px;
  }
}

bool ppu_checkOverscan(Ppu* ppu) {
  // called at (0,225)
  ppu->frameOverscan = PPU_overscan(ppu); // set if we have a overscan-frame
  return ppu->frameOverscan;
}

void ppu_handleVblank(Ppu* ppu) {
  // called either right after ppu_checkOverscan at (0,225), or at (0,240)
  if(!PPU_forcedBlank(ppu)) {
    ppu->oamAdr = ppu->oamaddl;
    ppu->oamInHigh = ppu->oamaddh & 1;
    ppu->oamSecondWrite = false;
  }
  ppu->frameInterlace = PPU_interlace(ppu); // set if we have a interlaced frame
}

static inline void ClearBackdrop(PpuPixelPrioBufs *buf) {
  for (size_t i = 0; i != arraysize(buf->data); i += 4)
    *(uint64*)&buf->data[i] = 0x0500050005000500;
}

// mosaicModulo is sized for the logical 256-wide screen, but widescreen window
// edges can fall in the border (negative on the left, >=256 on the right).
// Clamp the lookup so the rare mosaic+widescreen combination stays in-bounds;
// border mosaic alignment is approximate but never reads out of range. With
// extra==0 (authentic) every index is already in [0,256), so this is a no-op.
static inline uint8 PpuMosaicAt(Ppu *ppu, int i) {
  return ppu->mosaicModulo[(unsigned)i < (unsigned)kPpuXPixels ? i : (i < 0 ? 0 : kPpuXPixels - 1)];
}

void ppu_runLine(Ppu* ppu, int line) {
  if(line == 0) {
    // Always-on: snapshot the OAM the scanline renderer is about to consume.
    debug_server_on_oam_render();
    if (PPU_mosaicSize(ppu) != ppu->lastMosaicModulo) {
      int mod = PPU_mosaicSize(ppu);
      ppu->lastMosaicModulo = mod;
      for (int i = 0, j = 0; i < countof(ppu->mosaicModulo); i++) {
        ppu->mosaicModulo[i] = i - j;
        j = (j + 1 == mod ? 0 : j + 1);
      }
    }


    // pre-render line
    // TODO: this now happens halfway into the first line
    ppu->mosaicStartLine = 1;
    ppu->rangeOver = false;
    ppu->timeOver = false;
    ppu->evenFrame = !ppu->evenFrame;
  } else {  
    // Cache the brightness computation
    if (PPU_brightness(ppu) != ppu->lastBrightnessMult) {
      uint8_t ppu_brightness = PPU_brightness(ppu);
      ppu->lastBrightnessMult = ppu_brightness;
      for (int i = 0; i < 32; i++)
        ppu->brightnessMultHalf[i * 2] = ppu->brightnessMultHalf[i * 2 + 1] = ppu->brightnessMult[i] =
        ((i << 3) | (i >> 2)) * ppu_brightness / 15;
      // Store 31 extra entries to remove the need for clamping to 31.
      memset(&ppu->brightnessMult[32], ppu->brightnessMult[31], 31);
    }

    // evaluate sprites
    ClearBackdrop(&ppu->objBuffer);
    if (ppu->overlayRenderBuffer[kPpuOverlaySource_Obj])
      memset(&ppu->overlayBuffers[kPpuOverlaySource_Obj], 0,
             sizeof(ppu->overlayBuffers[kPpuOverlaySource_Obj]));
    ppu->lineHasSprites = !PPU_forcedBlank(ppu) && ppu_evaluateSprites(ppu, line - 1);

    if (g_new_ppu) {
      PpuDrawWholeLine(ppu, line);
    } else {
      PpuDrawWholeLineOldPpu(ppu, line);
    }
  }
}

typedef struct PpuWindows {
  // Up to five hardware-window spans plus two margin-gap splits.
  int16 edges[8];
  uint8 nr;
  uint8 bits;
} PpuWindows;

// Per-layer widescreen side margin. BG3 (layer 2) carries the HUD and is
// clamped to the authentic 256-wide region so a BG3 status bar never tiles into
// the margins -- EXCEPT on scanlines >= wsBg3WidenY, where the game renders
// level content on BG3 (e.g. SMW water) that should fill 16:9 like BG1/BG2.
static void PpuWindows_Clear(PpuWindows *win, Ppu *ppu, uint layer, int y) {
  win->edges[0] = -PpuWidescreenLayerExtra(ppu, layer, y, ppu->extraLeftCur);
  win->edges[1] = 256 + PpuWidescreenLayerExtra(ppu, layer, y,
                                                ppu->extraRightCur);
  win->nr = 1;
  win->bits = 0;
}

static void PpuWindows_Calc(PpuWindows *win, Ppu *ppu, uint layer, int y) {
  // Evaluate which spans to render based on the window settings.
  // There are at most 5 windows.
  // Algorithm from Snes9x
  uint32 winflags = GET_WINDOW_FLAGS(ppu, layer);
  uint nr = 1;
  int window_right = 256 + PpuWidescreenLayerExtra(ppu, layer, y,
                                                   ppu->extraRightCur);
  win->edges[0] = -PpuWidescreenLayerExtra(ppu, layer, y, ppu->extraLeftCur);
  win->edges[1] = window_right;
  uint i, j;
  int t;
  // A hardware edge pinned to 0/255 means "screen edge" in widescreen too.
  // This is a no-op at authentic width and prevents full-screen color/windows
  // from classifying otherwise valid side-margin pixels as outside.
  int w1l = ppu->window1left, w1r = ppu->window1right;
  int w2l = ppu->window2left, w2r = ppu->window2right;
  PpuWidescreenAdjustPinnedWindowEdges(win->edges[0], window_right, &w1l,
                                       &w1r, &w2l, &w2r);
  bool w1_ena = (winflags & kWindow1Enabled) && w1l <= w1r;
  if (w1_ena) {
    if (w1l > win->edges[0]) {
      win->edges[nr] = w1l;
      win->edges[++nr] = window_right;
    }
    if (w1r + 1 < window_right) {
      win->edges[nr] = w1r + 1;
      win->edges[++nr] = window_right;
    }
  }
  bool w2_ena = (winflags & kWindow2Enabled) && w2l <= w2r;
  if (w2_ena) {
    for (i = 0; i <= nr && (t = w2l) != win->edges[i]; i++) {
      if (t < win->edges[i]) {
        for (j = nr++; j >= i; j--)
          win->edges[j + 1] = win->edges[j];
        win->edges[i] = t;
        break;
      }
    }
    for (; i <= nr && (t = w2r + 1) != win->edges[i]; i++) {
      if (t < win->edges[i]) {
        for (j = nr++; j >= i; j--)
          win->edges[j + 1] = win->edges[j];
        win->edges[i] = t;
        break;
      }
    }
  }
  win->nr = nr;
  // get a bitmap of how regions map to windows
  uint8 w1_bits = 0, w2_bits = 0;
  if (w1_ena) {
    for (i = 0; win->edges[i] != w1l; i++);
    for (j = i; win->edges[j] != w1r + 1; j++);
    w1_bits = ((1 << (j - i)) - 1) << i;
  }
  if ((winflags & (kWindow1Enabled | kWindow1Inversed)) == (kWindow1Enabled | kWindow1Inversed))
    w1_bits = ~w1_bits;
  if (w2_ena) {
    for (i = 0; win->edges[i] != w2l; i++);
    for (j = i; win->edges[j] != w2r + 1; j++);
    w2_bits = ((1 << (j - i)) - 1) << i;
  }
  if ((winflags & (kWindow2Enabled | kWindow2Inversed)) == (kWindow2Enabled | kWindow2Inversed))
    w2_bits = ~w2_bits;
  win->bits = w1_bits | w2_bits;
}

static void PpuWindowsSplit(PpuWindows *win, int16 *bias, int xpos) {
  for (uint i = 0; i < win->nr; i++) {
    if (win->edges[i] < xpos && xpos < win->edges[i + 1]) {
      for (uint j = win->nr; j >= i + 1; j--)
        win->edges[j + 1] = win->edges[j];
      win->edges[i + 1] = (int16)xpos;
      for (uint j = win->nr - 1; j >= i + 1; j--)
        bias[j + 1] = bias[j];
      bias[i + 1] = bias[i];
      uint8 lo = win->bits & (uint8)((1u << (i + 1)) - 1);
      uint8 hi = (uint8)((win->bits >> (i + 1)) << (i + 2));
      win->bits = lo | hi | (uint8)(((win->bits >> i) & 1) << (i + 1));
      win->nr++;
      return;
    }
  }
}

static void PpuApplyMarginGap(Ppu *ppu, uint layer, PpuWindows *win,
                              int16 *bias) {
  int gl = ppu->wsMarginGapL[layer], gr = ppu->wsMarginGapR[layer];
  if (!(gl | gr) || !(ppu->extraLeftCur | ppu->extraRightCur))
    return;
  PpuWindowsSplit(win, bias, 0);
  PpuWindowsSplit(win, bias, 256);
  for (uint i = 0; i < win->nr; i++) {
    if (win->edges[i + 1] <= 0)
      bias[i] = (int16)(bias[i] - gl);
    else if (win->edges[i] >= 256)
      bias[i] = (int16)(bias[i] + gr);
  }
}

// Draw a whole line of a 4bpp background layer into bgBuffers
static void PpuDrawBackground_4bpp(Ppu *ppu, PpuPixelPrioBufs *dstbuf,
                                   uint y, bool sub, uint layer,
                                   PpuZbufType zhi, PpuZbufType zlo) {
#define BG1_MARGIN_ALLOWED(i) \
  (layer != 0 || !ppu->widescreenLineEnhancer || \
   (dstz + (i) >= dstbuf->data + kPpuExtraLeftRight && \
    dstz + (i) < dstbuf->data + kPpuExtraLeftRight + kPpuXPixels))
#define DO_PIXEL(i) do { \
  pixel = (bits >> i) & 1 | (bits >> (7 + i)) & 2 | (bits >> (14 + i)) & 4 | (bits >> (21 + i)) & 8; \
  if (BG1_MARGIN_ALLOWED(i) && (bits & (0x01010101 << i)) && z > dstz[i]) dstz[i] = z + pixel; } while (0)
#define DO_PIXEL_HFLIP(i) do { \
  pixel = (bits >> (7 - i)) & 1 | (bits >> (14 - i)) & 2 | (bits >> (21 - i)) & 4 | (bits >> (28 - i)) & 8; \
  if (BG1_MARGIN_ALLOWED(i) && (bits & (0x80808080 >> i)) && z > dstz[i]) dstz[i] = z + pixel; } while (0)
#define READ_BITS(ta, tile) (addr = &ppu->vram[((ta) + (tile) * 16) & 0x7fff], addr[0] | addr[8] << 16)
  enum { kPaletteShift = 6 };
  if (!IS_SCREEN_ENABLED(ppu, sub, layer))
    return;  // layer is completely hidden
  PpuWindows win;
  IS_SCREEN_WINDOWED(ppu, sub, layer) ? PpuWindows_Calc(&win, ppu, layer, y) : PpuWindows_Clear(&win, ppu, layer, y);
  int16 ws_bias[8] = { 0 };
  PpuApplyMarginGap(ppu, layer, &win, ws_bias);
  y += ppu->vScroll[layer];
  int sc_offs = PPU_bgTilemapAdr(ppu, layer) + (((y >> 3) & 0x1f) << 5);
  if ((y & 0x100) && PPU_bgTilemapHigher(ppu, layer))
    sc_offs += PPU_bgTilemapWider(ppu, layer) ? 0x800 : 0x400;
  const uint16 *tps[2] = {
    &ppu->vram[sc_offs & 0x7fff],
    &ppu->vram[sc_offs + (PPU_bgTilemapWider(ppu, layer) ? 0x400 : 0) & 0x7fff]
  };
  int tileadr = PPU_bgTileAdr(ppu, layer), pixel;
  int tileadr1 = tileadr + 7 - (y & 0x7), tileadr0 = tileadr + (y & 0x7);
  const uint16 *addr;
  bool ws_shadow = WsShadowLayerActive(layer);
#define WS_TILE(t, sx) (ws_shadow ? WsShadowTile(layer, (sx), y, (uint16_t)ppu->hScroll[layer], (uint16_t)(tp - ppu->vram), (uint16_t)(t)) : (uint32)(t))
  for (size_t windex = 0; windex < win.nr; windex++) {
    if (win.bits & (1 << windex))
      continue;  // layer is disabled for this window part
    uint x = win.edges[windex] + ppu->hScroll[layer] + ws_bias[windex];
    uint w = win.edges[windex + 1] - win.edges[windex];
    int ws_sx = win.edges[windex];
    PpuZbufType *dstz = dstbuf->data + win.edges[windex] + kPpuExtraLeftRight;
    const uint16 *tp = tps[x >> 8 & 1] + ((x >> 3) & 0x1f);
    const uint16 *tp_last = tps[x >> 8 & 1] + 31;
    const uint16 *tp_next = tps[(x >> 8 & 1) ^ 1];
#define NEXT_TP() if (tp != tp_last) tp += 1; else tp = tp_next, tp_next = tp_last - 31, tp_last = tp + 31;
    // Handle clipped pixels on left side
    if (x & 7) {
      int curw = IntMin(8 - (x & 7), w);
      w -= curw;
      uint32 tile = WS_TILE(*tp, ws_sx);
      ws_sx += curw;
      NEXT_TP();
      int ta = (tile & 0x8000) ? tileadr1 : tileadr0;
      PpuZbufType z = (tile & 0x2000) ? zhi : zlo;
      uint32 bits = READ_BITS(ta, tile & 0x3ff);
      if (bits) {
        z += ((tile & 0x1c00) >> kPaletteShift);
        if (tile & 0x4000) {
          bits >>= (x & 7), x += curw;
          do DO_PIXEL(0); while (bits >>= 1, dstz++, --curw);
        } else {
          bits <<= (x & 7), x += curw;
          do DO_PIXEL_HFLIP(0); while (bits <<= 1, dstz++, --curw);
        }
      } else {
        dstz += curw;
      }
    }
    // Handle full tiles in the middle
    while (w >= 8) {
      uint32 tile = WS_TILE(*tp, ws_sx);
      NEXT_TP();
      int ta = (tile & 0x8000) ? tileadr1 : tileadr0;
      PpuZbufType z = (tile & 0x2000) ? zhi : zlo;
      uint32 bits = READ_BITS(ta, tile & 0x3ff);
      if (bits) {
        z += ((tile & 0x1c00) >> kPaletteShift);
        if (tile & 0x4000) {
          DO_PIXEL(0); DO_PIXEL(1); DO_PIXEL(2); DO_PIXEL(3);
          DO_PIXEL(4); DO_PIXEL(5); DO_PIXEL(6); DO_PIXEL(7);
        } else {
          DO_PIXEL_HFLIP(0); DO_PIXEL_HFLIP(1); DO_PIXEL_HFLIP(2); DO_PIXEL_HFLIP(3);
          DO_PIXEL_HFLIP(4); DO_PIXEL_HFLIP(5); DO_PIXEL_HFLIP(6); DO_PIXEL_HFLIP(7);
        }
      }
      dstz += 8, w -= 8, ws_sx += 8;
    }
    // Handle remaining clipped part
    if (w) {
      uint32 tile = WS_TILE(*tp, ws_sx);
      int ta = (tile & 0x8000) ? tileadr1 : tileadr0;
      PpuZbufType z = (tile & 0x2000) ? zhi : zlo;
      uint32 bits = READ_BITS(ta, tile & 0x3ff);
      if (bits) {
        z += ((tile & 0x1c00) >> kPaletteShift);
        if (tile & 0x4000) {
          do DO_PIXEL(0); while (bits >>= 1, dstz++, --w);
        } else {
          do DO_PIXEL_HFLIP(0); while (bits <<= 1, dstz++, --w);
        }
      }
    }
  }
#undef WS_TILE
#undef READ_BITS
#undef DO_PIXEL
#undef DO_PIXEL_HFLIP
#undef BG1_MARGIN_ALLOWED
}

/* Draw an 8bpp tiled background (mode 3/4 BG1).  The original renderer only
 * implemented mode 1's 4bpp/2bpp layers and treated every other mode as mode
 * 7.  Super FX games commonly DMA their planar framebuffer into a mode-3 BG1,
 * so that fallback interpreted perfectly valid tile data as a mode-7 bitmap.
 * Keep this scalar for clarity; 256 pixels per line is insignificant beside
 * the coprocessor workload and gives us the complete 8x8/16x16 tile contract. */
static void PpuDrawBackground_8bpp(Ppu *ppu, uint y, bool sub, uint layer,
                                  PpuZbufType zhi, PpuZbufType zlo) {
  if (!IS_SCREEN_ENABLED(ppu, sub, layer))
    return;

  PpuWindows win;
  IS_SCREEN_WINDOWED(ppu, sub, layer)
      ? PpuWindows_Calc(&win, ppu, layer, y)
      : PpuWindows_Clear(&win, ppu, layer, y);

  const bool big = PPU_bigTiles(ppu, layer) != 0;
  const unsigned tile_shift = big ? 4 : 3;
  const unsigned page_shift = tile_shift + 5;
  const unsigned tile_mask = (1u << tile_shift) - 1u;
  const int tileadr = PPU_bgTileAdr(ppu, layer);
  const int mapadr = PPU_bgTilemapAdr(ppu, layer);
  const int sy = (int)y + ppu->vScroll[layer];

  for (size_t windex = 0; windex < win.nr; windex++) {
    if (win.bits & (1 << windex))
      continue;
    const int left = win.edges[windex];
    const int right = win.edges[windex + 1];
    PpuZbufType *dstz = ppu->bgBuffers[sub].data + left + kPpuExtraLeftRight;

    for (int screen_x = left; screen_x < right; screen_x++, dstz++) {
      const int sx = screen_x + ppu->hScroll[layer];
      int sc = mapadr + (((sy >> tile_shift) & 31) << 5);
      if (((sy >> page_shift) & 1) && PPU_bgTilemapHigher(ppu, layer))
        sc += PPU_bgTilemapWider(ppu, layer) ? 0x800 : 0x400;
      if (((sx >> page_shift) & 1) && PPU_bgTilemapWider(ppu, layer))
        sc += 0x400;
      sc += (sx >> tile_shift) & 31;

      uint16 tile = ppu->vram[sc & 0x7fff];
      unsigned px = (unsigned)sx & tile_mask;
      unsigned py = (unsigned)sy & tile_mask;
      if (tile & 0x4000) px = tile_mask - px;
      if (tile & 0x8000) py = tile_mask - py;

      unsigned character = tile & 0x3ff;
      if (big)
        character = (character + (px >> 3) + ((py >> 3) << 4)) & 0x3ff;
      const unsigned row = py & 7;
      const unsigned bit = 7 - (px & 7);
      const unsigned addr = (tileadr + character * 32 + row) & 0x7fff;
      const uint16 p01 = ppu->vram[addr];
      const uint16 p23 = ppu->vram[(addr + 8) & 0x7fff];
      const uint16 p45 = ppu->vram[(addr + 16) & 0x7fff];
      const uint16 p67 = ppu->vram[(addr + 24) & 0x7fff];
      const unsigned pixel =
          ((p01 >> bit) & 1) | (((p01 >> (bit + 8)) & 1) << 1) |
          (((p23 >> bit) & 1) << 2) | (((p23 >> (bit + 8)) & 1) << 3) |
          (((p45 >> bit) & 1) << 4) | (((p45 >> (bit + 8)) & 1) << 5) |
          (((p67 >> bit) & 1) << 6) | (((p67 >> (bit + 8)) & 1) << 7);
      const PpuZbufType z = (tile & 0x2000) ? zhi : zlo;
      if (pixel && z > *dstz)
        *dstz = z + pixel;
    }
  }
}

static uint16 PpuReadTilemapEntry(Ppu *ppu, uint layer, int tx, int ty) {
  int addr = PPU_bgTilemapAdr(ppu, layer) + ((ty & 31) << 5) + (tx & 31);
  if (((ty >> 5) & 1) && PPU_bgTilemapHigher(ppu, layer))
    addr += PPU_bgTilemapWider(ppu, layer) ? 0x800 : 0x400;
  if (((tx >> 5) & 1) && PPU_bgTilemapWider(ppu, layer))
    addr += 0x400;
  return ppu->vram[addr & 0x7fff];
}

/* Capture a native-width Mode 1 4bpp layer independently of the compositor.
 * Star Fox temporarily changes its 3D scene from Mode 2 to Mode 1 for flashes
 * and scripted effects while retaining the same landscape and GSU surface.
 * The normal wide renderer is intentionally allowed to evaluate tiles beyond
 * the SNES viewport, but those columns are not reliable source material for
 * presentation synthesis, so retain only the authentic x=0..255 result. */
static void PpuCaptureBackground_4bpp(Ppu *ppu, uint y, bool sub, uint layer) {
  uint8_t *capture = NULL;
  uint8_t *bg1_palette = NULL;
  if (!sub && ppu->wsMode2CaptureLayer == layer + 1 && y > 0 && y <= 224) {
    capture = ppu->wsMode2Capture[y - 1];
    memset(capture, 0, kPpuXPixels);
  }
  if (!sub && ppu->wsMode2CaptureLayer && layer == 0 && y > 0 && y <= 224) {
    bg1_palette = ppu->wsMode2Bg1Palette[y - 1];
    memset(bg1_palette, 0, kPpuXPixels);
  }
  if ((!capture && !bg1_palette) || !IS_SCREEN_ENABLED(ppu, sub, layer))
    return;

  PpuWindows win;
  IS_SCREEN_WINDOWED(ppu, sub, layer)
      ? PpuWindows_Calc(&win, ppu, layer, y)
      : PpuWindows_Clear(&win, ppu, layer, y);

  const bool big = PPU_bigTiles(ppu, layer) != 0;
  const unsigned tile_shift = big ? 4 : 3;
  const unsigned tile_mask = (1u << tile_shift) - 1u;
  const unsigned coord_mask = big ? 0x3ffu : 0x1ffu;
  const int tileadr = PPU_bgTileAdr(ppu, layer);
  const unsigned sy = (ppu->vScroll[layer] + y) & coord_mask;

  for (size_t windex = 0; windex < win.nr; windex++) {
    if (win.bits & (1 << windex))
      continue;
    const int left = IntMax(win.edges[windex], 0);
    const int right = IntMin(win.edges[windex + 1], kPpuXPixels);
    for (int screen_x = left; screen_x < right; screen_x++) {
      const unsigned sx =
          (ppu->hScroll[layer] + (unsigned)screen_x) & coord_mask;
      uint16 tile = PpuReadTilemapEntry(
          ppu, layer, sx >> tile_shift, sy >> tile_shift);
      unsigned px = sx & tile_mask;
      unsigned py = sy & tile_mask;
      if (tile & 0x4000) px = tile_mask - px;
      if (tile & 0x8000) py = tile_mask - py;
      unsigned character = tile & 0x3ff;
      if (big)
        character =
            (character + (px >> 3) + ((py >> 3) << 4)) & 0x3ff;
      const unsigned addr =
          (tileadr + character * 16 + (py & 7)) & 0x7fff;
      const uint16 p01 = ppu->vram[addr];
      const uint16 p23 = ppu->vram[(addr + 8) & 0x7fff];
      const unsigned bit = 7 - (px & 7);
      const unsigned pixel =
          ((p01 >> bit) & 1) | (((p01 >> (bit + 8)) & 1) << 1) |
          (((p23 >> bit) & 1) << 2) | (((p23 >> (bit + 8)) & 1) << 3);
      const uint8_t palette_base = (uint8_t)((tile & 0x1c00) >> 6);
      if (capture && pixel)
        capture[screen_x] = palette_base + (uint8_t)pixel;
      if (bg1_palette)
        bg1_palette[screen_x] = palette_base;
    }
  }
}

/* Modes 2/4/6 use BG3's tilemap as per-column scroll overrides for the
 * displayed layers.  Mode 2 (the Star Fox attract/game perspective screens)
 * has separate horizontal and vertical rows: BG3VOFS-1 selects the horizontal
 * row and BG3VOFS+7 the vertical row.  Bit 13 enables BG1, bit 14 enables BG2.
 * The first visible tile column cannot be offset on hardware. */
static void PpuDrawBackground_4bpp_opt(Ppu *ppu, uint y, bool sub, uint layer,
                                      PpuZbufType zhi, PpuZbufType zlo) {
  uint8_t *capture = NULL;
  uint8_t *bg1_palette = NULL;
  if (!sub && ppu->wsMode2CaptureLayer == layer + 1 && y > 0 && y <= 224) {
    capture = ppu->wsMode2Capture[y - 1];
    memset(capture, 0, kPpuXPixels);
  }
  if (!sub && ppu->wsMode2CaptureLayer && layer == 0 && y > 0 && y <= 224) {
    bg1_palette = ppu->wsMode2Bg1Palette[y - 1];
    memset(bg1_palette, 0, kPpuXPixels);
  }
  if (!IS_SCREEN_ENABLED(ppu, sub, layer))
    return;

  PpuWindows win;
  IS_SCREEN_WINDOWED(ppu, sub, layer)
      ? PpuWindows_Calc(&win, ppu, layer, y)
      : PpuWindows_Clear(&win, ppu, layer, y);

  const bool big = PPU_bigTiles(ppu, layer) != 0;
  const bool opt_big = PPU_bigTiles(ppu, 2) != 0;
  const unsigned tile_shift = big ? 4 : 3;
  const unsigned tile_mask = (1u << tile_shift) - 1u;
  const unsigned coord_mask = big ? 0x3ffu : 0x1ffu;
  const unsigned opt_shift = opt_big ? 4 : 3;
  const unsigned opt_coord_mask = opt_big ? 0x3ffu : 0x1ffu;
  const int tileadr = PPU_bgTileAdr(ppu, layer);
  const unsigned hscroll = ppu->hScroll[layer];
  const unsigned vscroll = ppu->vScroll[layer];
  const unsigned opt_hscroll = ppu->hScroll[2];
  const unsigned opt_vscroll = ppu->vScroll[2] - 1u;
  const int opt_hrow = (opt_vscroll & opt_coord_mask) >> opt_shift;
  const int opt_vrow = ((opt_vscroll + 8) & opt_coord_mask) >> opt_shift;
  const uint16 enable = (uint16)(0x2000u << layer);

  for (size_t windex = 0; windex < win.nr; windex++) {
    if (win.bits & (1 << windex))
      continue;
    const int left = win.edges[windex];
    const int right = win.edges[windex + 1];
    PpuZbufType *dstz =
        ppu->bgBuffers[sub].data + left + kPpuExtraLeftRight;
    bool left_edge = left < 8 - (hscroll & 7);

    /* OPT values apply to a rendered segment, not independently to every
     * screen pixel.  The selected horizontal offset determines where the
     * next eight-pixel source-tile boundary lies; only there does the PPU
     * fetch another BG3 offset-map entry. */
    for (int screen_x = left; screen_x < right;) {
      unsigned hoffset = hscroll;
      unsigned voffset = vscroll;
      if (left_edge) {
        /* The SNES cannot apply OPT to the leftmost source-tile column. */
        left_edge = false;
      } else {
        const unsigned opt_pos =
            (opt_hscroll + (unsigned)screen_x - 1) & opt_coord_mask;
        const int opt_x = opt_pos >> opt_shift;
        const uint16 hcell = PpuReadTilemapEntry(ppu, 2, opt_x, opt_hrow);
        const uint16 vcell = PpuReadTilemapEntry(ppu, 2, opt_x, opt_vrow);
        if (hcell & enable)
          hoffset = (hcell & ~7) | (hscroll & 7);
        if (vcell & enable)
          voffset = (uint16)(vcell + 1);
      }

      const unsigned sx = (hoffset + (unsigned)screen_x) & coord_mask;
      const unsigned sy = (voffset + y) & coord_mask;
      unsigned width = 8 - (sx & 7);
      if (width > (unsigned)(right - screen_x))
        width = right - screen_x;

      for (unsigned i = 0; i < width; i++, dstz++) {
        unsigned source_x = (sx + i) & coord_mask;
        uint16 tile = PpuReadTilemapEntry(
            ppu, layer, source_x >> tile_shift, sy >> tile_shift);
        unsigned px = source_x & tile_mask;
        unsigned py = sy & tile_mask;
        if (tile & 0x4000)
          px = tile_mask - px;
        if (tile & 0x8000)
          py = tile_mask - py;

        unsigned character = tile & 0x3ff;
        if (big)
          character =
              (character + (px >> 3) + ((py >> 3) << 4)) & 0x3ff;
        const unsigned addr =
            (tileadr + character * 16 + (py & 7)) & 0x7fff;
        const uint16 p01 = ppu->vram[addr];
        const uint16 p23 = ppu->vram[(addr + 8) & 0x7fff];
        const unsigned bit = 7 - (px & 7);
        const unsigned pixel =
            ((p01 >> bit) & 1) | (((p01 >> (bit + 8)) & 1) << 1) |
            (((p23 >> bit) & 1) << 2) |
            (((p23 >> (bit + 8)) & 1) << 3);
        const PpuZbufType z = (tile & 0x2000) ? zhi : zlo;
        const uint8_t palette_pixel =
            pixel ? (uint8_t)(((tile & 0x1c00) >> 6) + pixel) : 0;
        const int capture_x = screen_x + (int)i;
        if (capture && capture_x >= 0 && capture_x < kPpuXPixels)
          capture[capture_x] = palette_pixel;
        if (bg1_palette && capture_x >= 0 && capture_x < kPpuXPixels)
          bg1_palette[capture_x] = (uint8_t)((tile & 0x1c00) >> 6);
        if (palette_pixel && z > *dstz)
          *dstz = z + palette_pixel;
      }
      screen_x += width;
    }
  }
}

// Draw a whole line of a 2bpp background layer into bgBuffers
static void PpuDrawBackground_2bpp(Ppu *ppu, PpuPixelPrioBufs *dstbuf, uint y, bool sub, uint layer, PpuZbufType zhi, PpuZbufType zlo) {
#define DO_PIXEL(i) do { \
  pixel = (bits >> i) & 1 | (bits >> (7 + i)) & 2; \
  if (pixel && z > dstz[i]) dstz[i] = z + pixel; } while (0)
#define DO_PIXEL_HFLIP(i) do { \
  pixel = (bits >> (7 - i)) & 1 | (bits >> (14 - i)) & 2; \
  if (pixel && z > dstz[i]) dstz[i] = z + pixel; } while (0)
#define READ_BITS(ta, tile) (addr = &ppu->vram[(ta) + (tile) * 8 & 0x7fff], addr[0])
  enum { kPaletteShift = 8 };
  if (!IS_SCREEN_ENABLED(ppu, sub, layer))
    return;  // layer is completely hidden
  PpuWindows win;
  IS_SCREEN_WINDOWED(ppu, sub, layer) ? PpuWindows_Calc(&win, ppu, layer, y) : PpuWindows_Clear(&win, ppu, layer, y);
  // Widescreen HUD split (PpuSetWidescreenHudSplit): on HUD scanlines,
  // replace the single span with five — left chunk re-anchored to the left
  // border edge, a transparent gap, the centered chunk, a gap, and the
  // right chunk re-anchored to the right border edge. ws_bias shifts each
  // drawn span's source x so the chunks keep sampling their authentic
  // tilemap columns. Applied only when the computed window set is one
  // full drawn span: games (SMW) often enable screen-level window masking
  // ($212E) with no window selected for the layer, which still routes
  // through PpuWindows_Calc but degenerates to a single span. An actual
  // window shape (e.g. the level-start iris) keeps the authentic centered
  // HUD for those frames — split + real windows don't compose.
  int16 ws_bias[8] = { 0 };
  if (dstbuf == &ppu->bgBuffers[sub] &&
      layer == 2 && y < ppu->wsHudSplitHeight &&
      ppu->extraLeftRight &&
      win.nr == 1 && win.bits == 0) {
    win.nr = 5;
    win.bits = 0x0A;  // spans 1 and 3 are the gaps
    win.edges[0] = -ppu->extraLeftRight;
    win.edges[1] = ppu->wsHudLeftEnd - ppu->extraLeftRight;
    win.edges[2] = ppu->wsHudLeftEnd;
    win.edges[3] = ppu->wsHudRightStart;
    win.edges[4] = ppu->wsHudRightStart + ppu->extraLeftRight;
    win.edges[5] = 256 + ppu->extraLeftRight;
    ws_bias[0] = ppu->extraLeftRight;
    ws_bias[4] = -(int16)ppu->extraLeftRight;
  } else {
    PpuApplyMarginGap(ppu, layer, &win, ws_bias);
  }
  y += ppu->vScroll[layer];
  int sc_offs = PPU_bgTilemapAdr(ppu, layer) + (((y >> 3) & 0x1f) << 5);
  if ((y & 0x100) && PPU_bgTilemapHigher(ppu, layer))
    sc_offs += PPU_bgTilemapWider(ppu, layer) ? 0x800 : 0x400;
  const uint16 *tps[2] = {
    &ppu->vram[sc_offs & 0x7fff],
    &ppu->vram[sc_offs + (PPU_bgTilemapWider(ppu, layer) ? 0x400 : 0) & 0x7fff]
  };
  int tileadr = PPU_bgTileAdr(ppu, layer), pixel;
  int tileadr1 = tileadr + 7 - (y & 0x7), tileadr0 = tileadr + (y & 0x7);

  const uint16 *addr;
  for (size_t windex = 0; windex < win.nr; windex++) {
    if (win.bits & (1 << windex))
      continue;  // layer is disabled for this window part
    uint x = win.edges[windex] + ppu->hScroll[layer] + ws_bias[windex];
    uint w = win.edges[windex + 1] - win.edges[windex];
    PpuZbufType *dstz = dstbuf->data + win.edges[windex] + kPpuExtraLeftRight;
    const uint16 *tp = tps[x >> 8 & 1] + ((x >> 3) & 0x1f);
    const uint16 *tp_last = tps[x >> 8 & 1] + 31;
    const uint16 *tp_next = tps[(x >> 8 & 1) ^ 1];

#define NEXT_TP() if (tp != tp_last) tp += 1; else tp = tp_next, tp_next = tp_last - 31, tp_last = tp + 31;
    // Handle clipped pixels on left side
    if (x & 7) {
      int curw = IntMin(8 - (x & 7), w);
      w -= curw;
      uint32 tile = *tp;
      NEXT_TP();
      int ta = (tile & 0x8000) ? tileadr1 : tileadr0;
      PpuZbufType z = (tile & 0x2000) ? zhi : zlo;
      uint32 bits = READ_BITS(ta, tile & 0x3ff);
      if (bits) {
        z += ((tile & 0x1c00) >> kPaletteShift);
        if (tile & 0x4000) {
          bits >>= (x & 7), x += curw;
          do DO_PIXEL(0); while (bits >>= 1, dstz++, --curw);
        } else {
          bits <<= (x & 7), x += curw;
          do DO_PIXEL_HFLIP(0); while (bits <<= 1, dstz++, --curw);
        }
      } else {
        dstz += curw;
      }
    }
    // Handle full tiles in the middle
    while (w >= 8) {
      uint32 tile = *tp;
      NEXT_TP();
      int ta = (tile & 0x8000) ? tileadr1 : tileadr0;
      PpuZbufType z = (tile & 0x2000) ? zhi : zlo;
      uint32 bits = READ_BITS(ta, tile & 0x3ff);
      if (bits) {
        z += ((tile & 0x1c00) >> kPaletteShift);
        if (tile & 0x4000) {
          DO_PIXEL(0); DO_PIXEL(1); DO_PIXEL(2); DO_PIXEL(3);
          DO_PIXEL(4); DO_PIXEL(5); DO_PIXEL(6); DO_PIXEL(7);
        } else {
          DO_PIXEL_HFLIP(0); DO_PIXEL_HFLIP(1); DO_PIXEL_HFLIP(2); DO_PIXEL_HFLIP(3);
          DO_PIXEL_HFLIP(4); DO_PIXEL_HFLIP(5); DO_PIXEL_HFLIP(6); DO_PIXEL_HFLIP(7);
        }
      }
      dstz += 8, w -= 8;
    }
    // Handle remaining clipped part
    if (w) {
      uint32 tile = *tp;
      int ta = (tile & 0x8000) ? tileadr1 : tileadr0;
      PpuZbufType z = (tile & 0x2000) ? zhi : zlo;
      uint32 bits = READ_BITS(ta, tile & 0x3ff);
      if (bits) {
        z += ((tile & 0x1c00) >> kPaletteShift);
        if (tile & 0x4000) {
          do DO_PIXEL(0); while (bits >>= 1, dstz++, --w);
        } else {
          do DO_PIXEL_HFLIP(0); while (bits <<= 1, dstz++, --w);
        }
      }
    }
  }
#undef NEXT_TP
#undef READ_BITS
#undef DO_PIXEL
#undef DO_PIXEL_HFLIP
}


// Draw a whole line of a 4bpp background layer into bgBuffers, with mosaic applied
static void PpuDrawBackground_4bpp_mosaic(Ppu *ppu,
                                          PpuPixelPrioBufs *dstbuf, uint y,
                                          bool sub, uint layer,
                                          PpuZbufType zhi, PpuZbufType zlo) {
#define BG1_MARGIN_ALLOWED(i) \
  (layer != 0 || !ppu->widescreenLineEnhancer || \
   (dstz + (i) >= dstbuf->data + kPpuExtraLeftRight && \
    dstz + (i) < dstbuf->data + kPpuExtraLeftRight + kPpuXPixels))
#define GET_PIXEL() pixel = (bits) & 1 | (bits >> 7) & 2 | (bits >> 14) & 4 | (bits >> 21) & 8
#define GET_PIXEL_HFLIP() pixel = (bits >> 7) & 1 | (bits >> 14) & 2 | (bits >> 21) & 4 | (bits >> 28) & 8
#define READ_BITS(ta, tile) (addr = &ppu->vram[((ta) + (tile) * 16) & 0x7fff], addr[0] | addr[8] << 16)
  enum { kPaletteShift = 6 };
  if (!IS_SCREEN_ENABLED(ppu, sub, layer))
    return;  // layer is completely hidden
  PpuWindows win;
  IS_SCREEN_WINDOWED(ppu, sub, layer) ? PpuWindows_Calc(&win, ppu, layer, y) : PpuWindows_Clear(&win, ppu, layer, y);
  y = ppu->mosaicModulo[y] + ppu->vScroll[layer];
  int sc_offs = PPU_bgTilemapAdr(ppu, layer) + (((y >> 3) & 0x1f) << 5);
  if ((y & 0x100) && PPU_bgTilemapHigher(ppu, layer))
    sc_offs += PPU_bgTilemapWider(ppu, layer) ? 0x800 : 0x400;
  const uint16 *tps[2] = {
    &ppu->vram[sc_offs & 0x7fff],
    &ppu->vram[sc_offs + (PPU_bgTilemapWider(ppu, layer) ? 0x400 : 0) & 0x7fff]
  };
  int tileadr = PPU_bgTileAdr(ppu, layer), pixel;
  int tileadr1 = tileadr + 7 - (y & 0x7), tileadr0 = tileadr + (y & 0x7);
  const uint16 *addr;
  bool ws_shadow = WsShadowLayerActive(layer);
#define WS_TILE(t, sx) (ws_shadow ? WsShadowTile(layer, (sx), y, (uint16_t)ppu->hScroll[layer], (uint16_t)(tp - ppu->vram), (uint16_t)(t)) : (uint32)(t))
  for (size_t windex = 0; windex < win.nr; windex++) {
    if (win.bits & (1 << windex))
      continue;  // layer is disabled for this window part
    int sx = win.edges[windex];
    int ws_sx = sx;
    PpuZbufType *dstz = dstbuf->data + sx + kPpuExtraLeftRight;
    PpuZbufType *dstz_end = dstbuf->data + win.edges[windex + 1] + kPpuExtraLeftRight;
    uint x = sx + ppu->hScroll[layer];
    const uint16 *tp = tps[x >> 8 & 1] + ((x >> 3) & 0x1f);
    const uint16 *tp_last = tps[x >> 8 & 1] + 31, *tp_next = tps[(x >> 8 & 1) ^ 1];
    x &= 7;
    int mosaic_size = PPU_mosaicSize(ppu);
    int w = mosaic_size - (sx - PpuMosaicAt(ppu, sx));
    do {
      w = IntMin(w, dstz_end - dstz);
      uint32 tile = WS_TILE(*tp, ws_sx);
      int ta = (tile & 0x8000) ? tileadr1 : tileadr0;
      PpuZbufType z = (tile & 0x2000) ? zhi : zlo;
      uint32 bits = READ_BITS(ta, tile & 0x3ff);
      if (tile & 0x4000) bits >>= x, GET_PIXEL(); else bits <<= x, GET_PIXEL_HFLIP();
      if (pixel) {
        pixel += (tile & 0x1c00) >> kPaletteShift;
        int i = 0;
        do {
          if (BG1_MARGIN_ALLOWED(i) && z > dstz[i])
            dstz[i] = pixel + z;
        } while (++i != w);
      }
      dstz += w, x += w, ws_sx += w;
      for (; x >= 8; x -= 8)
        tp = (tp != tp_last) ? tp + 1 : tp_next;
      w = mosaic_size;
    } while (dstz_end - dstz != 0);
  }
#undef WS_TILE
#undef READ_BITS
#undef GET_PIXEL
#undef GET_PIXEL_HFLIP
#undef BG1_MARGIN_ALLOWED
}

// Merge one isolated layer into the live priority buffer, padding only that
// layer's side margins so transparent pixels never duplicate lower layers or
// sprites. `repeat` selects cyclic continuation; otherwise reflect the edge.
static void PpuMergePaddedBackground(Ppu *ppu, PpuPixelPrioBufs *dstbuf,
                                     const PpuPixelPrioBufs *layerbuf,
                                     bool repeat, bool full_budget) {
  PpuZbufType *dst = dstbuf->data;
  const PpuZbufType *src = layerbuf->data;
  int left_extra = full_budget ? ppu->extraLeftRight : ppu->extraLeftCur;
  int right_extra = full_budget ? ppu->extraLeftRight : ppu->extraRightCur;
  for (int x = 0; x < kPpuXPixels; x++) {
    int i = x + kPpuExtraLeftRight;
    if (src[i] > dst[i]) dst[i] = src[i];
  }
  for (int x = -left_extra; x < 0; x++) {
    int di = x + kPpuExtraLeftRight;
    int sx = repeat ? kPpuXPixels + x : -x;
    int si = sx + kPpuExtraLeftRight;
    if (src[si] > dst[di]) dst[di] = src[si];
  }
  for (int x = kPpuXPixels;
       x < kPpuXPixels + right_extra; x++) {
    int di = x + kPpuExtraLeftRight;
    int sx = repeat ? x - kPpuXPixels : kPpuXPixels * 2 - 2 - x;
    int si = sx + kPpuExtraLeftRight;
    if (src[si] > dst[di]) dst[di] = src[si];
  }
}

static void PpuMergeStretchedBackground(Ppu *ppu, PpuPixelPrioBufs *dstbuf,
                                        const PpuPixelPrioBufs *layerbuf) {
  PpuZbufType *dst = dstbuf->data;
  const PpuZbufType *src = layerbuf->data;
  const int left_extra = ppu->extraLeftRight;
  const int right_extra = ppu->extraLeftRight;
  const int out_width = kPpuXPixels + left_extra + right_extra;
  if (out_width <= 0)
    return;

  for (int x = -left_extra; x < kPpuXPixels + right_extra; x++) {
    int di = x + kPpuExtraLeftRight;
    int sx = ((x + left_extra) * kPpuXPixels) / out_width;
    if (sx < 0) sx = 0;
    if (sx >= kPpuXPixels) sx = kPpuXPixels - 1;
    int si = sx + kPpuExtraLeftRight;
    if (src[si] > dst[di]) dst[di] = src[si];
  }
}

static void PpuDrawBackground_4bpp_policy(Ppu *ppu, PpuPixelPrioBufs *dstbuf,
                                          uint y, bool sub,
                                          uint layer, PpuZbufType zhi,
                                          PpuZbufType zlo, bool mosaic) {
  uint8_t padding = ppu->wsLayerMirror | ppu->wsLayerRepeat;
  bool repeat_band = PpuWidescreenLayerRepeatBandActive(ppu, layer, y);
  bool stretch_band = PpuWidescreenLayerStretchBandActive(ppu, layer, y);
  if (!(padding & (1u << layer)) && !repeat_band && !stretch_band) {
    if (mosaic)
      PpuDrawBackground_4bpp_mosaic(ppu, dstbuf, y, sub,
                                    layer, zhi, zlo);
    else
      PpuDrawBackground_4bpp(ppu, dstbuf, y, sub, layer,
                             zhi, zlo);
    return;
  }

  PpuPixelPrioBufs layerbuf;
  ClearBackdrop(&layerbuf);
  if (mosaic)
    PpuDrawBackground_4bpp_mosaic(ppu, &layerbuf, y, sub, layer, zhi, zlo);
  else
    PpuDrawBackground_4bpp(ppu, &layerbuf, y, sub, layer, zhi, zlo);
  if (stretch_band) {
    PpuMergeStretchedBackground(ppu, dstbuf, &layerbuf);
  } else {
    PpuMergePaddedBackground(ppu, dstbuf, &layerbuf,
                             repeat_band ||
                             (ppu->wsLayerRepeat & (1u << layer)) != 0,
                             repeat_band);
  }
}

static void PpuDrawBackground_2bpp_mosaic(Ppu *ppu, PpuPixelPrioBufs *dstbuf,
                                          int y, bool sub, uint layer,
                                          PpuZbufType zhi, PpuZbufType zlo);

static void PpuDrawBackground_2bpp_policy(Ppu *ppu, PpuPixelPrioBufs *dstbuf,
                                          uint y, bool sub,
                                          uint layer, PpuZbufType zhi,
                                          PpuZbufType zlo, bool mosaic) {
  uint8_t padding = ppu->wsLayerMirror | ppu->wsLayerRepeat;
  bool repeat_band = PpuWidescreenLayerRepeatBandActive(ppu, layer, y);
  bool stretch_band = PpuWidescreenLayerStretchBandActive(ppu, layer, y);
  if (!(padding & (1u << layer)) && !repeat_band && !stretch_band) {
    if (mosaic)
      PpuDrawBackground_2bpp_mosaic(ppu, dstbuf, y, sub, layer, zhi, zlo);
    else
      PpuDrawBackground_2bpp(ppu, dstbuf, y, sub, layer, zhi, zlo);
    return;
  }

  PpuPixelPrioBufs layerbuf;
  ClearBackdrop(&layerbuf);
  if (mosaic)
    PpuDrawBackground_2bpp_mosaic(ppu, &layerbuf, y, sub, layer, zhi, zlo);
  else
    PpuDrawBackground_2bpp(ppu, &layerbuf, y, sub, layer, zhi, zlo);
  if (stretch_band) {
    PpuMergeStretchedBackground(ppu, dstbuf, &layerbuf);
  } else {
    PpuMergePaddedBackground(ppu, dstbuf, &layerbuf,
                             repeat_band ||
                             (ppu->wsLayerRepeat & (1u << layer)) != 0,
                             repeat_band);
  }
}

// Draw a whole line of a 2bpp background layer into bgBuffers, with mosaic applied
static void PpuDrawBackground_2bpp_mosaic(Ppu *ppu, PpuPixelPrioBufs *dstbuf, int y, bool sub, uint layer, PpuZbufType zhi, PpuZbufType zlo) {
#define GET_PIXEL() pixel = (bits) & 1 | (bits >> 7) & 2
#define GET_PIXEL_HFLIP() pixel = (bits >> 7) & 1 | (bits >> 14) & 2
#define READ_BITS(ta, tile) (addr = &ppu->vram[((ta) + (tile) * 8) & 0x7fff], addr[0])
  enum { kPaletteShift = 8 };
  if (!IS_SCREEN_ENABLED(ppu, sub, layer))
    return;  // layer is completely hidden
  PpuWindows win;
  IS_SCREEN_WINDOWED(ppu, sub, layer) ? PpuWindows_Calc(&win, ppu, layer, y) : PpuWindows_Clear(&win, ppu, layer, y);
  y = ppu->mosaicModulo[y] + ppu->vScroll[layer];
  int sc_offs = PPU_bgTilemapAdr(ppu, layer) + (((y >> 3) & 0x1f) << 5);
  if ((y & 0x100) && PPU_bgTilemapHigher(ppu, layer))
    sc_offs += PPU_bgTilemapWider(ppu, layer) ? 0x800 : 0x400;
  const uint16 *tps[2] = {
    &ppu->vram[sc_offs & 0x7fff],
    &ppu->vram[sc_offs + (PPU_bgTilemapWider(ppu, layer) ? 0x400 : 0) & 0x7fff]
  };
  int tileadr = PPU_bgTileAdr(ppu, layer), pixel;
  int tileadr1 = tileadr + 7 - (y & 0x7), tileadr0 = tileadr + (y & 0x7);
  const uint16 *addr;
  for (size_t windex = 0; windex < win.nr; windex++) {
    if (win.bits & (1 << windex))
      continue;  // layer is disabled for this window part
    int sx = win.edges[windex];
    PpuZbufType *dstz = dstbuf->data + sx + kPpuExtraLeftRight;
    PpuZbufType *dstz_end = dstbuf->data + win.edges[windex + 1] + kPpuExtraLeftRight;
    uint x = sx + ppu->hScroll[layer];
    const uint16 *tp = tps[x >> 8 & 1] + ((x >> 3) & 0x1f);
    const uint16 *tp_last = tps[x >> 8 & 1] + 31, *tp_next = tps[(x >> 8 & 1) ^ 1];
    x &= 7;
    int mosaic_size = PPU_mosaicSize(ppu);
    int w = mosaic_size - (sx - PpuMosaicAt(ppu, sx));
    do {
      w = IntMin(w, dstz_end - dstz);
      uint32 tile = *tp;
      int ta = (tile & 0x8000) ? tileadr1 : tileadr0;
      PpuZbufType z = (tile & 0x2000) ? zhi : zlo;
      uint32 bits = READ_BITS(ta, tile & 0x3ff);
      if (tile & 0x4000) bits >>= x, GET_PIXEL(); else bits <<= x, GET_PIXEL_HFLIP();
      if (pixel) {
        pixel += (tile & 0x1c00) >> kPaletteShift;
        uint i = 0;
        do {
          if (z > dstz[i])
            dstz[i] = pixel + z;
        } while (++i != w);
      }
      dstz += w, x += w;
      for (; x >= 8; x -= 8)
        tp = (tp != tp_last) ? tp + 1 : tp_next;
      w = mosaic_size;
    } while (dstz_end - dstz != 0);
  }
#undef READ_BITS
#undef GET_PIXEL
#undef GET_PIXEL_HFLIP
}


// Assumes it's drawn on an empty backdrop
static void PpuDrawBackground_mode7(Ppu *ppu, PpuPixelPrioBufs *dstbuf, uint y, bool sub, PpuZbufType z) {
  int layer = 0;
  if (!IS_SCREEN_ENABLED(ppu, sub, layer))
    return;  // layer is completely hidden
  PpuWindows win;
  IS_SCREEN_WINDOWED(ppu, sub, layer) ? PpuWindows_Calc(&win, ppu, layer, y) : PpuWindows_Clear(&win, ppu, layer, y);

  // expand 13-bit values to signed values
  int hScroll = ((int16_t)(ppu->m7matrix[6] << 3)) >> 3;
  int vScroll = ((int16_t)(ppu->m7matrix[7] << 3)) >> 3;
  int xCenter = ((int16_t)(ppu->m7matrix[4] << 3)) >> 3;
  int yCenter = ((int16_t)(ppu->m7matrix[5] << 3)) >> 3;
  int clippedH = hScroll - xCenter;
  int clippedV = vScroll - yCenter;
  clippedH = (clippedH & 0x2000) ? (clippedH | ~1023) : (clippedH & 1023);
  clippedV = (clippedV & 0x2000) ? (clippedV | ~1023) : (clippedV & 1023);
  uint8 mosaic_enabled = PPU_mosaicEnabled(ppu, 0);
  if (mosaic_enabled)
    y = ppu->mosaicModulo[y];
  uint32 ry = PPU_m7yFlip(ppu) ? 255 - y : y;
  uint32 m7startX = (ppu->m7matrix[0] * clippedH & ~63) + (ppu->m7matrix[1] * ry & ~63) +
    (ppu->m7matrix[1] * clippedV & ~63) + (xCenter << 8);
  uint32 m7startY = (ppu->m7matrix[2] * clippedH & ~63) + (ppu->m7matrix[3] * ry & ~63) +
    (ppu->m7matrix[3] * clippedV & ~63) + (yCenter << 8);
  for (size_t windex = 0; windex < win.nr; windex++) {
    if (win.bits & (1 << windex))
      continue;  // layer is disabled for this window part
    int x = win.edges[windex], x2 = win.edges[windex + 1], tile;
    PpuZbufType *dstz = dstbuf->data + x + kPpuExtraLeftRight;
    PpuZbufType *dstz_end = dstbuf->data + x2 + kPpuExtraLeftRight;
    uint32 rx = PPU_m7xFlip(ppu) ? 255 - x : x;
    uint32 xpos = m7startX + ppu->m7matrix[0] * rx;
    uint32 ypos = m7startY + ppu->m7matrix[2] * rx;
    uint32 dx = PPU_m7xFlip(ppu) ? -ppu->m7matrix[0] : ppu->m7matrix[0];
    uint32 dy = PPU_m7xFlip(ppu) ? -ppu->m7matrix[2] : ppu->m7matrix[2];
    uint32 outside_value = PPU_m7largeField(ppu) ? 0x3ffff : 0xffffffff;
    bool char_fill = PPU_m7charFill(ppu);
    if (mosaic_enabled) {
      int w = PPU_mosaicSize(ppu) - (x - PpuMosaicAt(ppu, x));
      do {
        w = IntMin(w, dstz_end - dstz);
        if ((uint32)(xpos | ypos) > outside_value) {
          if (!char_fill)
            continue;
          tile = 0;
        } else {
          tile = ppu->vram[(ypos >> 11 & 0x7f) * 128 + (xpos >> 11 & 0x7f)] & 0xff;
        }
        uint8 pixel = ppu->vram[tile * 64 + (ypos >> 8 & 7) * 8 + (xpos >> 8 & 7)] >> 8;
        if (pixel) {
          int i = 0;
          do dstz[i] = pixel + z; while (++i != w);
        }
      } while (xpos += dx * w, ypos += dy * w, dstz += w, w = PPU_mosaicSize(ppu), dstz_end - dstz != 0);
    } else {
      do {
        if ((uint32)(xpos | ypos) > outside_value) {
          if (!char_fill)
            continue;
          tile = 0;
        } else {
          tile = ppu->vram[(ypos >> 11 & 0x7f) * 128 + (xpos >> 11 & 0x7f)] & 0xff;
        }
        uint8 pixel = ppu->vram[tile * 64 + (ypos >> 8 & 7) * 8 + (xpos >> 8 & 7)] >> 8;
        if (pixel)
          dstz[0] = pixel + z;
      } while (xpos += dx, ypos += dy, ++dstz != dstz_end);
    }
  }
}

static void PpuDrawSprites(Ppu *ppu, uint y, uint sub, bool clear_backdrop) {
  int layer = 4;
  if (!IS_SCREEN_ENABLED(ppu, sub, layer))
    return;  // layer is completely hidden
  PpuWindows win;
  IS_SCREEN_WINDOWED(ppu, sub, layer) ? PpuWindows_Calc(&win, ppu, layer, y) : PpuWindows_Clear(&win, ppu, layer, y);
  for (size_t windex = 0; windex < win.nr; windex++) {
    if (win.bits & (1 << windex))
      continue;  // layer is disabled for this window part
    int left = win.edges[windex];
    int width = win.edges[windex + 1] - left;
    PpuZbufType *src = ppu->objBuffer.data + left + kPpuExtraLeftRight;
    PpuZbufType *dst = ppu->bgBuffers[sub].data + left + kPpuExtraLeftRight;
    if (clear_backdrop) {
      memcpy(dst, src, width * sizeof(uint16));
    } else {
      do {
        if (src[0] > dst[0])
          dst[0] = src[0];
      } while (src++, dst++, --width);
    }
  }
}

static bool PpuOverlayActiveOnLine(Ppu *ppu, PpuOverlaySource source,
                                   int screen_y) {
  if ((unsigned)source >= kPpuOverlaySource_Count ||
      !ppu->overlayRenderBuffer[source] ||
      !ppu->overlayRenderPitch[source])
    return false;
  const PpuOverlayCapture *capture = &ppu->overlayCaptures[source];
  return capture->x1 > capture->x0 && capture->y1 > capture->y0 &&
         screen_y >= capture->y0 && screen_y < capture->y1;
}

static uint32 PpuOverlayColor(Ppu *ppu, PpuZbufType pixel) {
  /* Isolated layer buffers may contain the priority-only backdrop marker.
   * A zero palette index is still transparent for a captured BG/OBJ plane. */
  if (!(pixel & 0xff)) return 0;
  uint32 color = ppu->cgram[pixel & 0xff];
  return 0xff000000u |
      (uint32)ppu->brightnessMult[color & 0x1f] << 16 |
      (uint32)ppu->brightnessMult[(color >> 5) & 0x1f] << 8 |
      ppu->brightnessMult[(color >> 10) & 0x1f];
}

static void PpuClearOverlayRenderLine(Ppu *ppu, uint y) {
  if (y == 0) return;
  int screen_y = (int)y - 1;
  for (int source = 0; source < kPpuOverlaySource_Count; source++) {
    uint8_t *pixels = ppu->overlayRenderBuffer[source];
    uint32_t pitch = ppu->overlayRenderPitch[source];
    if (pixels && pitch)
      memset(pixels + (size_t)screen_y * pitch, 0, pitch);
  }
}

static void PpuWriteOverlayRenderLine(Ppu *ppu, PpuOverlaySource source,
                                      uint y) {
  if (y == 0) return;
  int screen_y = (int)y - 1;
  if (!PpuOverlayActiveOnLine(ppu, source, screen_y))
    return;

  uint32_t pitch = ppu->overlayRenderPitch[source];
  int width = (int)(pitch / sizeof(uint32));
  int texture_extra = IntMax((width - kPpuXPixels) / 2, 0);
  int screen_min = -texture_extra;
  int screen_max = width - texture_extra;
  const PpuOverlayCapture *capture = &ppu->overlayCaptures[source];
  int x0 = IntMax(capture->x0, screen_min);
  int x1 = IntMin(capture->x1, screen_max);
  if (x1 <= x0 || x0 + kPpuExtraLeftRight < 0 ||
      x1 + kPpuExtraLeftRight > kPpuBufWidth)
    return;

  uint32 *dst = (uint32 *)(ppu->overlayRenderBuffer[source] +
                            (size_t)screen_y * pitch);
  const PpuZbufType *src = ppu->overlayBuffers[source].data;
  for (int x = x0; x < x1; x++)
    dst[x + texture_extra] =
        PpuOverlayColor(ppu, src[x + kPpuExtraLeftRight]);
}

// Choose the destination priority buffer for a background layer. When the
// layer's overlay source is active on this line, draw it in isolation into a
// dedicated buffer (so it can be captured and optionally removed). Otherwise
// return the normal composition buffer, leaving the draw path byte-identical.
static PpuPixelPrioBufs *PpuBeginBackgroundOverlay(Ppu *ppu, uint y,
                                                   bool sub, uint layer) {
  int screen_y = (int)y - 1;
  PpuOverlaySource source = (PpuOverlaySource)layer;
  if (!PpuOverlayActiveOnLine(ppu, source, screen_y))
    return &ppu->bgBuffers[sub];
  memset(&ppu->overlayBuffers[source], 0,
         sizeof(ppu->overlayBuffers[source]));
  return &ppu->overlayBuffers[source];
}

// Merge an isolated overlay layer back into the composition buffer (skipping
// the captured rectangle when RemoveFromGame is set) and export the captured
// pixels to the bound host surface. A no-op when the layer was drawn straight
// into bgBuffers (overlay inactive).
static void PpuFinishBackgroundOverlay(Ppu *ppu, uint y, bool sub,
                                       uint layer,
                                       PpuPixelPrioBufs *layerbuf) {
  PpuOverlaySource source = (PpuOverlaySource)layer;
  if (layerbuf != &ppu->overlayBuffers[source])
    return;

  if (!sub)
    PpuWriteOverlayRenderLine(ppu, source, y);

  const PpuOverlayCapture *capture = &ppu->overlayCaptures[source];
  PpuZbufType *dst = ppu->bgBuffers[sub].data;
  const PpuZbufType *src = layerbuf->data;
  bool remove = (capture->flags & kPpuOverlayFlag_RemoveFromGame) != 0;
  for (int i = 0; i < kPpuBufWidth; i++) {
    int x = i - kPpuExtraLeftRight;
    if (remove && x >= capture->x0 && x < capture->x1)
      continue;
    if (src[i] > dst[i])
      dst[i] = src[i];
  }
}

static void PpuDrawBackgrounds(Ppu *ppu, int y, bool sub) {
  // Top 4 bits contain the prio level, and bottom 4 bits the layer type.
  // SPRITE_PRIO_TO_PRIO can be used to convert from obj prio to this prio.
  //  15: BG3 tiles with priority 1 if bit 3 of $2105 is set
  //  14: Sprites with priority 3 (4 * sprite_prio + 2)
  //  12: BG1 tiles with priority 1
  //  11: BG2 tiles with priority 1
  //  10: Sprites with priority 2 (4 * sprite_prio + 2)
  //  8: BG1 tiles with priority 0
  //  7: BG2 tiles with priority 0
  //  6: Sprites with priority 1 (4 * sprite_prio + 2)
  //  3: BG3 tiles with priority 1 if bit 3 of $2105 is clear
  //  2: Sprites with priority 0 (4 * sprite_prio + 2)
  //  1: BG3 tiles with priority 0
  //  0: backdrop

  if (PPU_mode(ppu) == 1) {
    if (ppu->lineHasSprites)
      PpuDrawSprites(ppu, y, sub, true);

    if (ppu->wsMode2CaptureLayer) {
      PpuCaptureBackground_4bpp(ppu, y, sub, 0);
      PpuCaptureBackground_4bpp(ppu, y, sub, 1);
    }

    bool mosaic_size = PPU_mosaicSize(ppu) > 1;
    PpuPixelPrioBufs *layerbuf = PpuBeginBackgroundOverlay(ppu, y, sub, 0);
    PpuDrawBackground_4bpp_policy(
        ppu, layerbuf, y, sub, 0, 0xc000, 0x8000,
        mosaic_size && PPU_mosaicEnabled(ppu, 0));
    PpuFinishBackgroundOverlay(ppu, y, sub, 0, layerbuf);

    layerbuf = PpuBeginBackgroundOverlay(ppu, y, sub, 1);
    PpuDrawBackground_4bpp_policy(
        ppu, layerbuf, y, sub, 1, 0xb100, 0x7100,
        mosaic_size && PPU_mosaicEnabled(ppu, 1));
    PpuFinishBackgroundOverlay(ppu, y, sub, 1, layerbuf);

    uint bg3prio = PPU_bg3priority(ppu) ? 0xf200 : 0x3200;
    layerbuf = PpuBeginBackgroundOverlay(ppu, y, sub, 2);
    PpuDrawBackground_2bpp_policy(
        ppu, layerbuf, y, sub, 2, bg3prio, 0x1200,
        mosaic_size && PPU_mosaicEnabled(ppu, 2));
    PpuFinishBackgroundOverlay(ppu, y, sub, 2, layerbuf);
  } else if (PPU_mode(ppu) == 2) {
    if (ppu->lineHasSprites)
      PpuDrawSprites(ppu, y, sub, true);
    PpuDrawBackground_4bpp_opt(ppu, y, sub, 0, 0xc000, 0x8000);
    PpuDrawBackground_4bpp_opt(ppu, y, sub, 1, 0xb100, 0x7100);
  } else if (PPU_mode(ppu) == 3) {
    if (ppu->lineHasSprites)
      PpuDrawSprites(ppu, y, sub, true);
    PpuDrawBackground_8bpp(ppu, y, sub, 0, 0xc000, 0x8000);
    PpuDrawBackground_4bpp(ppu, &ppu->bgBuffers[sub], y, sub, 1,
                           0xb100, 0x7100);
  } else {
    // mode 7
    PpuPixelPrioBufs *layerbuf = PpuBeginBackgroundOverlay(ppu, y, sub, 0);
    PpuDrawBackground_mode7(ppu, layerbuf, y, sub, 0x5000);
    PpuFinishBackgroundOverlay(ppu, y, sub, 0, layerbuf);
    if (ppu->lineHasSprites)
      PpuDrawSprites(ppu, y, sub, false);
  }
}

static NOINLINE void PpuDrawWholeLine(Ppu *ppu, uint y) {
  PpuClearOverlayRenderLine(ppu, y);
  if (PPU_forcedBlank(ppu)) {
    uint8 *dst = &ppu->renderBuffer[(y - 1) * ppu->renderPitch];
    size_t n = sizeof(uint32) * (256 + ppu->extraLeftRight * 2);
    memset(dst, 0, n);
    return;
  }

  // Default background is backdrop
  ClearBackdrop(&ppu->bgBuffers[0]);

  // Render main screen
  PpuDrawBackgrounds(ppu, y, false);
  if (ppu->widescreenLineEnhancer &&
      (ppu->extraLeftCur || ppu->extraRightCur))
    ppu->widescreenLineEnhancer(ppu, y, false,
                                ppu->widescreenLineEnhancerContext);

  // Render also the subscreen?
  bool rendered_subscreen = false;
  if (PPU_preventMathMode(ppu) != 3 && PPU_addSubscreen(ppu) && PPU_mathEnabled(ppu)) {
    ClearBackdrop(&ppu->bgBuffers[1]);
    if (ppu->screenEnabled[1] != 0) {
      PpuDrawBackgrounds(ppu, y, true);
      if (ppu->widescreenLineEnhancer &&
          (ppu->extraLeftCur || ppu->extraRightCur))
        ppu->widescreenLineEnhancer(ppu, y, true,
                                    ppu->widescreenLineEnhancerContext);
      rendered_subscreen = true;
    }
  }

  // Color window affects the drawing mode in each region
  PpuWindows cwin;
  PpuWindows_Calc(&cwin, ppu, 5, y);
  bool compose_full_budget = false;
  if (ppu->wsHudSplitHeight && y < ppu->wsHudSplitHeight &&
      ppu->extraLeftRight && cwin.nr == 1 && cwin.bits == 0) {
    cwin.edges[0] = -(int16_t)ppu->extraLeftRight;
    cwin.edges[1] = 256 + ppu->extraLeftRight;
    compose_full_budget = true;
  } else if (PpuWidescreenLineRepeatBandActive(ppu, y) &&
             ppu->extraLeftRight && cwin.nr == 1 && cwin.bits == 0) {
    cwin.edges[0] = -(int16_t)ppu->extraLeftRight;
    cwin.edges[1] = 256 + ppu->extraLeftRight;
    compose_full_budget = true;
  }
  static const uint8 kCwBitsMod[8] = {
    0x00, 0xff, 0xff, 0x00,
    0xff, 0x00, 0xff, 0x00,
  };
  uint32 cw_clip_math = ((cwin.bits & kCwBitsMod[PPU_clipMode(ppu)]) ^ kCwBitsMod[PPU_clipMode(ppu) + 4]) |
    ((cwin.bits & kCwBitsMod[PPU_preventMathMode(ppu)]) ^ kCwBitsMod[PPU_preventMathMode(ppu) + 4]) << 8;

  uint32 *dst = (uint32*)&ppu->renderBuffer[(y - 1) * ppu->renderPitch], *dst_org = dst;

  dst += compose_full_budget ? 0 : (ppu->extraLeftRight - ppu->extraLeftCur);

  uint32 windex = 0;
  do {
    uint32 left = cwin.edges[windex] + kPpuExtraLeftRight, right = cwin.edges[windex + 1] + kPpuExtraLeftRight;
    // If clip is set, then zero out the rgb values from the main screen.
    uint32 clip_color_mask = (cw_clip_math & 1) ? 0x1f : 0;
    uint32 math_enabled_cur = PPU_mathEnabled(ppu) & ((cw_clip_math & 0x100) ? -1 : 0);
    uint32 fixed_color = ppu->fixedColor;
    if (math_enabled_cur == 0 || fixed_color == 0 && !PPU_halfColor(ppu) && !rendered_subscreen) {
      // Math is disabled (or has no effect), so can avoid the per-pixel maths check
      uint32 i = left;
      do {
        uint32 color = ppu->cgram[ppu->bgBuffers[0].data[i] & 0xff];
        dst[0] = ppu->brightnessMult[color & clip_color_mask] << 16 |
          ppu->brightnessMult[(color >> 5) & clip_color_mask] << 8 |
          ppu->brightnessMult[(color >> 10) & clip_color_mask];
      } while (dst++, ++i < right);
    } else {
      uint8 *half_color_map = PPU_halfColor(ppu) ? ppu->brightnessMultHalf : ppu->brightnessMult;
      // Store this in locals
      math_enabled_cur |= PPU_addSubscreen(ppu) << 8 | PPU_subtractColor(ppu) << 9;
      // Need to check for each pixel whether to use math or not based on the main screen layer.
      uint32 i = left;
      do {
        uint32 color = ppu->cgram[ppu->bgBuffers[0].data[i] & 0xff], color2;
        uint8 main_layer = (ppu->bgBuffers[0].data[i] >> 8) & 0xf;
        uint32 r = color & clip_color_mask;
        uint32 g = (color >> 5) & clip_color_mask;
        uint32 b = (color >> 10) & clip_color_mask;
        uint8 *color_map = ppu->brightnessMult;
        if (math_enabled_cur & (1 << main_layer)) {
          if (math_enabled_cur & 0x100) {  // addSubscreen ?
            if ((ppu->bgBuffers[1].data[i] & 0xff) != 0)
              color2 = ppu->cgram[ppu->bgBuffers[1].data[i] & 0xff], color_map = half_color_map;
            else  // Don't halve if PPU_addSubscreen(ppu) && backdrop
              color2 = fixed_color;
          } else {
            color2 = fixed_color, color_map = half_color_map;
          }
          uint32 r2 = (color2 & 0x1f), g2 = ((color2 >> 5) & 0x1f), b2 = ((color2 >> 10) & 0x1f);
          if (math_enabled_cur & 0x200) {  // subtractColor?
            r = (r >= r2) ? r - r2 : 0;
            g = (g >= g2) ? g - g2 : 0;
            b = (b >= b2) ? b - b2 : 0;
          } else {
            r += r2;
            g += g2;
            b += b2;
          }
        }
        dst[0] = color_map[b] | color_map[g] << 8 | color_map[r] << 16;
      } while (dst++, ++i < right);
    }
  } while (cw_clip_math >>= 1, ++windex < cwin.nr);

  PpuWriteOverlayRenderLine(ppu, kPpuOverlaySource_Obj, y);
}

static int PpuAdjustWidescreenHudOamX(Ppu *ppu, uint8_t index, uint8_t y,
                                      int x) {
  uint8_t slot = index >> 1;
  bool hud_y = ppu->wsHudSplitHeight &&
      (y >= 224 || y < ppu->wsHudSplitHeight);
  if (hud_y && ppu->wsHudOamSlots &&
      slot >= ppu->wsHudOamFirstSlot &&
      slot < ppu->wsHudOamFirstSlot + ppu->wsHudOamSlots) {
    if (x >= 0 && x < ppu->wsHudLeftEnd)
      x -= ppu->extraLeftCur;
    else if (x >= ppu->wsHudRightStart && x < 256)
      x += ppu->extraRightCur;
  }
  return x;
}

static bool ppu_evaluateSprites(Ppu* ppu, int line) {
  static const uint8 spriteSizes[8][2] = {
    {8, 16}, {8, 32}, {8, 64}, {16, 32},
    {16, 64}, {32, 64}, {16, 32}, {16, 32}
  };

  // TODO: rectangular sprites, wierdness with sprites at -256
  uint8_t index = PPU_objPriority(ppu) ? (ppu->oamaddl & 0xfe) : 0;
  int spritesFound = 0;
  int tilesFound = 0;
  uint8_t foundSprites[128];

  // Range evaluation walks OAM forward, but tile fetching walks the accepted
  // sprites backward. This is observable when the 34-sliver limit is reached.
  for(int i = 0; i < 128; i++) {
    uint8_t y = ppu->oam[index] >> 8;
    uint8_t row = line - y;
    int spriteSize = spriteSizes[PPU_objSize(ppu)][(ppu->highOam[index >> 3] >> ((index & 7) + 1)) & 1];
    int spriteHeight = PPU_objInterlace(ppu) ? spriteSize / 2 : spriteSize;
    if(row < spriteHeight) {
      int x = ppu->oam[index] & 0xff;
      x |= ((ppu->highOam[index >> 3] >> (index & 7)) & 1) << 8;
      if (x >= 256 + ppu->extraRightCur) x -= 512;
      x = PpuAdjustWidescreenHudOamX(ppu, index, y, x);
      if(x + spriteSize > -ppu->extraLeftCur) {
        spritesFound++;
        if(spritesFound > 32 &&
           !(ppu->renderFlags & kPpuRenderFlags_NoSpriteLimits)) {
          ppu->rangeOver = true;
          spritesFound = 32;
          break;
        }
        foundSprites[spritesFound - 1] = index;
      }
    }
    index += 2;
  }

  for(int i = spritesFound; i > 0; i--) {
    index = foundSprites[i - 1];
    uint8_t row = line - (ppu->oam[index] >> 8);
    int spriteSize = spriteSizes[PPU_objSize(ppu)][(ppu->highOam[index >> 3] >> ((index & 7) + 1)) & 1];
    int x = ppu->oam[index] & 0xff;
    x |= ((ppu->highOam[index >> 3] >> (index & 7)) & 1) << 8;
    if (x >= 256 + ppu->extraRightCur) x -= 512;
    x = PpuAdjustWidescreenHudOamX(ppu, index, ppu->oam[index] >> 8, x);
        if(PPU_objInterlace(ppu)) row = row * 2 + (ppu->evenFrame ? 0 : 1);
        int oam1 = ppu->oam[index + 1];
        int objAdr = (oam1 & 0x100) ? PPU_objTileAdr2(ppu) : PPU_objTileAdr1(ppu);
        if(oam1 & 0x8000) row = spriteSize - 1 - row;
        int paletteBase = 0x80 + 16 * ((oam1 & 0xe00) >> 9);
        int prio = SPRITE_PRIO_TO_PRIO((oam1 & 0x3000) >> 12, (oam1 & 0x800) == 0);
        PpuZbufType z = paletteBase + (prio << 8);

        for(int col = 0; col < spriteSize; col += 8) {
      if(col + x <= -8 - ppu->extraLeftCur ||
         col + x >= 256 + ppu->extraRightCur)
        continue;
            tilesFound++;
            if(tilesFound > 34 &&
               !(ppu->renderFlags & kPpuRenderFlags_NoSpriteLimits)) {
              ppu->timeOver = true;
              break;
            }
            int usedCol = oam1 & 0x4000 ? spriteSize - 1 - col : col;
      int usedTile = ((((oam1 & 0xff) >> 4) + (row >> 3)) << 4) |
                     (((oam1 & 0xf) + (usedCol >> 3)) & 0xf);
            uint16 *addr = &ppu->vram[(objAdr + usedTile * 16 + (row & 0x7)) & 0x7fff];
            uint32 plane = addr[0] | addr[8] << 16;
            int px_left = IntMax(-(col + x + kPpuExtraLeftRight), 0);
            int px_right = IntMin(256 + kPpuExtraLeftRight - (col + x), 8);
            PpuZbufType *dst = ppu->objBuffer.data + col + x + px_left + kPpuExtraLeftRight;
            int slot = index >> 1;
            PpuOverlayCapture *obj_capture =
                &ppu->overlayCaptures[kPpuOverlaySource_Obj];
            bool capture_slot = PpuOverlayActiveOnLine(
          ppu, kPpuOverlaySource_Obj, line) && obj_capture->oamCount &&
          slot >= obj_capture->oamFirst &&
                slot < obj_capture->oamFirst + obj_capture->oamCount;
            bool obj_remove = capture_slot &&
                (obj_capture->flags & kPpuOverlayFlag_RemoveFromGame) != 0;

            for (int px = px_left; px < px_right; px++, dst++) {
              int shift = oam1 & 0x4000 ? px : 7 - px;
              uint32 bits = plane >> shift;
        int pixel = (bits >> 0) & 1 | (bits >> 7) & 2 |
                    (bits >> 14) & 4 | (bits >> 21) & 8;
        if (pixel == 0) continue;
              if (capture_slot) {
                int screen_x = col + x + px;
                if (screen_x >= obj_capture->x0 && screen_x < obj_capture->x1) {
                  PpuZbufType *overlay =
                      &ppu->overlayBuffers[kPpuOverlaySource_Obj]
                           .data[dst - ppu->objBuffer.data];
                    *overlay = z + pixel;
            if (obj_remove) continue;
                }
              }
        // Lower OAM indices are processed later and overwrite higher ones.
                dst[0] = z + pixel;
            }
        }
        if(tilesFound > 34 &&
           !(ppu->renderFlags & kPpuRenderFlags_NoSpriteLimits))
      break;
  }
  return tilesFound != 0;
}

static uint16_t ppu_getVramRemap(Ppu* ppu) {
  uint16_t adr = ppu->vramPointer;
  switch(ppu->vramRemapMode) {
    case 0: return adr;
    case 1: return (adr & 0xff00) | ((adr & 0xe0) >> 5) | ((adr & 0x1f) << 3);
    case 2: return (adr & 0xfe00) | ((adr & 0x1c0) >> 6) | ((adr & 0x3f) << 3);
    case 3: return (adr & 0xfc00) | ((adr & 0x380) >> 7) | ((adr & 0x7f) << 3);
  }
  return adr;
}

uint8_t ppu_read(Ppu* ppu, uint8_t adr) {
  switch(adr) {
  case 0x34:
  case 0x35:
  case 0x36: {
    int result = ppu->m7matrix[0] * (ppu->m7matrix[1] >> 8);
    return (result >> (8 * (adr - 0x34))) & 0xff;
  }
    case 0x37: {
      /* SLHV latches the live beam. LLE advances Snes::hPos/vPos from the
       * shared master-clock timeline; hard-coding a convenient scanline made
       * games waiting for any other line loop forever. */
      ppu->hCount = g_snes->hPos / 4;
      ppu->vCount = g_snes->vPos;
      ppu->hCountSecond = false;
      ppu->vCountSecond = false;
      ppu->countersLatched = true;
      return 0;
    }
    case 0x38: {
      uint8_t ret = 0;
      if(ppu->oamInHigh) {
        ret = ppu->highOam[((ppu->oamAdr & 0xf) << 1) | ppu->oamSecondWrite];
        if(ppu->oamSecondWrite) {
          ppu->oamAdr++;
          if(ppu->oamAdr == 0) ppu->oamInHigh = false;
        }
      } else {
        if(!ppu->oamSecondWrite) {
          ret = ppu->oam[ppu->oamAdr] & 0xff;
        } else {
          ret = ppu->oam[ppu->oamAdr++] >> 8;
          if(ppu->oamAdr == 0) ppu->oamInHigh = true;
        }
      }
      ppu->oamSecondWrite = !ppu->oamSecondWrite;
      return ret;
    }
    case 0x39: {
      uint16_t val = ppu->vramReadBuffer;
      if(!ppu->vramIncrementOnHigh) {
        ppu->vramReadBuffer = ppu->vram[ppu_getVramRemap(ppu) & 0x7fff];
        ppu->vramPointer += ppu->vramIncrement;
      }
      return val & 0xff;
    }
    case 0x3a: {
      uint16_t val = ppu->vramReadBuffer;
      if(ppu->vramIncrementOnHigh) {
        ppu->vramReadBuffer = ppu->vram[ppu_getVramRemap(ppu) & 0x7fff];
        ppu->vramPointer += ppu->vramIncrement;
      }
      return val >> 8;
    }
    case 0x3b: {
      uint8_t ret = 0;
      if(!ppu->cgramSecondWrite) {
        ret = ppu->cgram[ppu->cgramPointer] & 0xff;
      } else {
        ret = ((ppu->cgram[ppu->cgramPointer++] >> 8) & 0x7f);
      }
      ppu->cgramSecondWrite = !ppu->cgramSecondWrite;
      return ret;
    }
    case 0x3c: {
      uint8_t val = 0;
      if(ppu->hCountSecond) {
        val = ((ppu->hCount >> 8) & 1);
      } else {
        val = ppu->hCount & 0xff;
      }
      ppu->hCountSecond = !ppu->hCountSecond;
      return val;
    }
    case 0x3d: {
      uint8_t val = 0;
      if(ppu->vCountSecond) {
        val = ((ppu->vCount >> 8) & 1);
      } else {
        val = ppu->vCount & 0xff;
      }
      ppu->vCountSecond = !ppu->vCountSecond;
      return val;
    }
    case 0x3e: {
      uint8_t val = 0x1; // ppu1 version (4 bit)
      val |= ppu->rangeOver << 6;
      val |= ppu->timeOver << 7;
      return val;
    }
    case 0x3f: {
      uint8_t val = 0x3; // ppu2 version (4 bit), bit 4: ntsc/pal
      val |= ppu->countersLatched << 6;
      val |= ppu->evenFrame << 7;
      ppu->countersLatched = false; // TODO: only when ppulatch is set
      ppu->hCountSecond = false;
      ppu->vCountSecond = false;
      return val;
    }
    default: {
      assert(0);
      return 0;
    }
  }
}

void ppu_write(Ppu* ppu, uint8_t adr, uint8_t val) {
//  if (adr != 24 && adr != 25)
//    printf("ppu_write(%d, %d)\n", adr, val);
  switch(adr) {
    case INIDISP & 0xff:
      ppu->inidisp = val;
      break;
    case OBSEL & 0xff:
      ppu->obsel = val;
      break;
    case OAMADDL & 0xff:
      ppu->oamaddl = val;
      ppu->oamAdr = val;
      ppu->oamInHigh = ppu->oamaddh & 1;
      ppu->oamSecondWrite = false;
      break;
    case OAMADDH & 0xff:
      ppu->oamaddh = val;
      ppu->oamInHigh = val & 1;
      ppu->oamAdr = ppu->oamaddl;
      ppu->oamSecondWrite = false;
      break;
    case 0x04: {
      if(ppu->oamInHigh) {
        int hidx = ((ppu->oamAdr & 0xf) << 1) | ppu->oamSecondWrite;
        ppu->highOam[hidx] = val;
        debug_server_on_oam_write(1, (uint16_t)hidx, (uint16_t)val);
        if(ppu->oamSecondWrite) {
          ppu->oamAdr++;
          if(ppu->oamAdr == 0) ppu->oamInHigh = false;
        }
      } else {
        if(!ppu->oamSecondWrite) {
          ppu->oamBuffer = val;
        } else {
          uint16_t widx = ppu->oamAdr;
          uint16_t word = (uint16_t)((val << 8) | ppu->oamBuffer);
          ppu->oam[ppu->oamAdr++] = word;
          debug_server_on_oam_write(0, widx, word);
          if(ppu->oamAdr == 0) ppu->oamInHigh = true;
        }
      }
      ppu->oamSecondWrite = !ppu->oamSecondWrite;
      break;
    }
    case BGMODE & 0xff:
      assert((val & 0xf0) == 0);
      ppu->bgmode = val;
      break;
    case MOSAIC & 0xff:
      ppu->mosaic = val;
      ppu->mosaicStartLine = 0;// ppu->snes->vPos;
      break;
    case BG1SC & 0xff:
    case BG2SC & 0xff:
    case BG3SC & 0xff:
    case BG4SC & 0xff:
      ppu->bgXsc[adr - 7] = val;
      break;
    case BG12NBA & 0xff:
      ppu->bgTileAdr = ppu->bgTileAdr & 0xff00 | val;
      break;
    case BG34NBA & 0xff:
      ppu->bgTileAdr = ppu->bgTileAdr & 0xff | val << 8;
      break;
    case 0x0d: {
      ppu->m7matrix[6] = ((val << 8) | ppu->m7prev) & 0x1fff;
      ppu->m7prev = val;
      // fallthrough to normal layer BG-HOFS
    }
    case 0x0f:
    case 0x11:
    case 0x13: {
      ppu->hScroll[(adr - 0xd) / 2] = ((val << 8) | (ppu->scrollPrev & 0xf8) | (ppu->scrollPrev2 & 0x7)) & 0x3ff;
      ppu->scrollPrev = val;
      ppu->scrollPrev2 = val;
      break;
    }
    case 0x0e: {
      ppu->m7matrix[7] = ((val << 8) | ppu->m7prev) & 0x1fff;
      ppu->m7prev = val;
      // fallthrough to normal layer BG-VOFS
    }
    case 0x10:
    case 0x12:
    case 0x14: {
      ppu->vScroll[(adr - 0xe) / 2] = ((val << 8) | ppu->scrollPrev) & 0x3ff;
      ppu->scrollPrev = val;
      break;
    }
    case 0x15: {
      if((val & 3) == 0) {
        ppu->vramIncrement = 1;
      } else if((val & 3) == 1) {
        ppu->vramIncrement = 32;
      } else {
        ppu->vramIncrement = 128;
      }
      ppu->vramRemapMode = (val & 0xc) >> 2;
      ppu->vramIncrementOnHigh = val & 0x80;
      break;
    }
    case 0x16: {
      ppu->vramPointer = (ppu->vramPointer & 0xff00) | val;
      ppu->vramReadBuffer = ppu->vram[ppu_getVramRemap(ppu) & 0x7fff];
      break;
    }
    case 0x17: {
      ppu->vramPointer = (ppu->vramPointer & 0x00ff) | (val << 8);
      ppu->vramReadBuffer = ppu->vram[ppu_getVramRemap(ppu) & 0x7fff];
      break;
    }
    case 0x18: {
      // TODO: vram access during rendering (also cgram and oam)
      uint16_t vramAdr = ppu_getVramRemap(ppu);
      ppu->vram[vramAdr & 0x7fff] = (ppu->vram[vramAdr & 0x7fff] & 0xff00) | val;
      // $2118 == low byte of word; byte_addr = word << 1.
      debug_server_on_vram_write(((uint32_t)(vramAdr & 0x7fff) << 1), val);
      WsShadowOnVramWrite((uint16_t)(vramAdr & 0x7fff),
                          ppu->vram[vramAdr & 0x7fff]);
      if(!ppu->vramIncrementOnHigh) ppu->vramPointer += ppu->vramIncrement;
      break;
    }
    case 0x19: {
      uint16_t vramAdr = ppu_getVramRemap(ppu);
      ppu->vram[vramAdr & 0x7fff] = (ppu->vram[vramAdr & 0x7fff] & 0x00ff) | (val << 8);
      // $2119 == high byte of word; byte_addr = (word << 1) + 1.
      debug_server_on_vram_write(((uint32_t)(vramAdr & 0x7fff) << 1) + 1, val);
      WsShadowOnVramWrite((uint16_t)(vramAdr & 0x7fff),
                          ppu->vram[vramAdr & 0x7fff]);
      if(ppu->vramIncrementOnHigh) ppu->vramPointer += ppu->vramIncrement;
      break;
    }
    case M7SEL & 0xff:
      ppu->m7sel = val;
      break;
    case 0x1b:
    case 0x1c:
    case 0x1d:
    case 0x1e:
      ppu->m7matrix[adr - 0x1b] = (val << 8) | ppu->m7prev;
      ppu->m7prev = val;
      break;
    case 0x1f:
    case 0x20:
      ppu->m7matrix[adr - 0x1b] = ((val << 8) | ppu->m7prev) & 0x1fff;
      ppu->m7prev = val;
      break;
    case 0x21:
      ppu->cgramPointer = val;
      ppu->cgramSecondWrite = false;
      break;
    case 0x22:
      if(!ppu->cgramSecondWrite) {
        ppu->cgramBuffer = val;
      } else {
        ppu->cgram[ppu->cgramPointer++] = (val << 8) | ppu->cgramBuffer;
      }
      ppu->cgramSecondWrite = !ppu->cgramSecondWrite;
      break;
    case 0x23:
      ppu->windowsel = (ppu->windowsel & ~0xff) | val;
      break;
    case 0x24:
      ppu->windowsel = (ppu->windowsel & ~0xff00) | (val << 8);
      break;
    case 0x25:
      ppu->windowsel = (ppu->windowsel & ~0xff0000) | (val << 16);
      break;
    case 0x26:
      ppu->window1left = val;
      break;
    case 0x27:
      ppu->window1right = val;
      break;
    case 0x28:
      ppu->window2left = val;
      break;
    case 0x29:
      ppu->window2right = val;
      break;
    case WBGLOG & 0xff:
      ppu->wbgobjlog = ppu->wbgobjlog & 0xff00 | val;
      break;
    case WOBJLOG & 0xff:
      ppu->wbgobjlog = ppu->wbgobjlog & 0xff | val << 8;
      break;
    case TM & 0xff:
      ppu->screenEnabled[0] = val;
      break;
    case TS & 0xff:
      ppu->screenEnabled[1] = val;
      break;
    case TMW & 0xff:
      ppu->screenWindowed[0] = val;
      break;
    case TSW & 0xff:
      ppu->screenWindowed[1] = val;
      break;
    case CGWSEL & 0xff:
      ppu->cgwsel = val;
      break;
    case CGADSUB & 0xff:
      ppu->cgadsub = val;
      break;
    case COLDATA & 0xff:
      if (val & 0x80) ppu->fixedColor = (ppu->fixedColor & ~(0x1f << 10)) | (val & 0x1f) << 10; // blue
      if (val & 0x40) ppu->fixedColor = (ppu->fixedColor & ~(0x1f <<  5)) | (val & 0x1f) << 5;  // green
      if (val & 0x20) ppu->fixedColor = (ppu->fixedColor & ~(0x1f <<  0)) | (val & 0x1f) << 0;  // red
      break;
    case SETINI & 0xff:
      ppu->setini = val;
      break;
    default:
      break;
  }
}

int PpuGetCurrentRenderScale(Ppu *ppu, uint32_t render_flags) {
  return 1;
}
