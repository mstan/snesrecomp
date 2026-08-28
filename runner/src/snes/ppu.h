#ifndef PPU_H
#define PPU_H

#include "../types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "saveload.h"

typedef struct Ppu Ppu;
typedef void PpuWidescreenLineEnhancer(Ppu *ppu, uint y, bool sub,
                                       void *context);

typedef struct BgLayer {
  uint16_t xhScroll;
  uint16_t xvScroll;
  bool xtilemapWider;
  bool xtilemapHigher;
  uint16_t xtilemapAdr;
  uint16_t xtileAdr;
  bool xxbigTiles;
  bool xxmosaicEnabled;
} BgLayer;

enum {
  kPpuXPixels = 256,
  // Maximum widescreen expansion *per side*, baked into the priority-buffer
  // capacity. This is a compile-time ceiling only; the actual extra columns
  // rendered each frame are the runtime ppu->extraLeftCur/extraRightCur, which
  // default to 0 (authentic 256-wide output). 96 per side allows up to a
  // 448-pixel internal width, comfortably past 16:9 at 224 lines.
  kPpuExtraLeftRight = 96,
  // Full internal width of the priority buffers (logical 256 + both borders).
  kPpuBufWidth = kPpuXPixels + kPpuExtraLeftRight * 2,
  // Split-screen games can assign distinct anchor layouts to each viewport.
  kPpuWsAnchorBands = 2,
};

typedef uint16_t PpuZbufType;

typedef struct PpuPixelPrioBufs {
  // This holds the prio in the upper 8 bits and the color in the lower 8 bits.
  // Sized for the widescreen border; logical screen x maps to
  // data[x + kPpuExtraLeftRight].
  PpuZbufType data[kPpuBufWidth];
} PpuPixelPrioBufs;

static inline void PpuWidescreenAdjustPinnedWindowEdges(
    int screen_left, int screen_right, int *w1l, int *w1r, int *w2l,
    int *w2r) {
  if (screen_left == 0 && screen_right == kPpuXPixels)
    return;
  if (*w1l == 0) *w1l = screen_left;
  if (*w1r == kPpuXPixels - 1) *w1r = screen_right - 1;
  if (*w2l == 0) *w2l = screen_left;
  if (*w2r == kPpuXPixels - 1) *w2r = screen_right - 1;
}

/* Renderer-neutral host-overlay extraction. BG source values deliberately
 * match the PPU layer indices; OBJ is the fifth screen layer. Each source can
 * own one screen-space capture rectangle and one full-frame ARGB destination
 * surface. A caller can crop several independently placed graphics from one
 * captured bounding rectangle after scanout. */
typedef enum PpuOverlaySource {
  kPpuOverlaySource_Bg1 = 0,
  kPpuOverlaySource_Bg2 = 1,
  kPpuOverlaySource_Bg3 = 2,
  kPpuOverlaySource_Bg4 = 3,
  kPpuOverlaySource_Obj = 4,
  kPpuOverlaySource_Count = 5,
} PpuOverlaySource;

enum {
  /* Do not merge captured pixels back into either main or subscreen. The host
   * can then reinsert them without a duplicate remaining in renderBuffer. */
  kPpuOverlayFlag_RemoveFromGame = 1,
};

typedef struct PpuOverlayCapture {
  /* SNES screen coordinates after scroll/window/mosaic processing. X may be
   * negative or exceed 255 when a widescreen margin is active. Endpoints are
   * exclusive. y uses visible output coordinates (0 is the first scanline). */
  int16_t x0, x1;
  int16_t y0, y1;
  uint8_t flags;
  /* OBJ-only selector. A zero count captures no objects. Games validate any
   * semantic identity (HUD icon, portrait, etc.) before supplying the range. */
  uint8_t oamFirst, oamCount;
} PpuOverlayCapture;

enum {
  kPpuRenderFlags_NewRenderer = 1,
  // Preserve the established bit value for config/save compatibility.
  kPpuRenderFlags_NoSpriteLimits = 8,
};

typedef struct Layer {
  bool xmainScreenEnabled;
  bool xsubScreenEnabled;
  bool xmainScreenWindowed;
  bool xsubScreenWindowed;
} Layer;

typedef struct WindowLayer {
  bool xwindow1enabled;
  bool xwindow2enabled;
  bool xwindow1inversed;
  bool xwindow2inversed;
  uint8_t xmaskLogic;
} WindowLayer;

#define PPU_SAVESTATE_REGS_SIZE 0x40
#define PPU_SAVESTATE_MEM_SIZE 0x10420

struct Ppu {
  // Snes registers. Saved to snapshot. Need to be stable
  // -- START OF SNAPSHOT, 0x40 bytes
  uint8 inidisp;
  uint8 obsel;
  uint8 oamaddl;
  uint8 oamaddh;
  uint8 bgmode;
  uint8 mosaic;
  uint8 bgXsc[4];
  uint16 bgTileAdr;
  uint8 m7sel;
  uint8 setini;
  uint16 hScroll[4];
  uint16 vScroll[4];
  int16_t m7matrix[8]; // a, b, c, d, x, y, h, v
  uint16 fixedColor;
  uint32 windowsel;
  uint8 window1left;
  uint8 window1right;
  uint8 window2left;
  uint8 window2right;
  uint16 wbgobjlog;
  uint8 screenEnabled[2];
  uint8 screenWindowed[2];
  uint8 cgadsub;
  uint8 cgwsel;
  // -- END OF SNAPSHOT

  // vram access
  uint16_t vramPointer;
  bool vramIncrementOnHigh;
  uint8_t vramRemapMode;
  uint8_t vramIncrement;
  uint16_t vramReadBuffer;
  // cgram access
  uint8_t cgramPointer;
  bool cgramSecondWrite;
  uint8_t cgramBuffer;
  // oam access
  uint8_t oamAdr;
  bool oamInHigh;
  bool oamSecondWrite;
  uint8_t oamBuffer;
  bool timeOver;
  bool rangeOver;
  uint8_t scrollPrev;
  uint8_t scrollPrev2;
  uint8_t mosaicStartLine;
  uint8_t m7prev;
  // mode 7 internal
  int32_t m7startX;
  int32_t m7startY;
  // settings
  bool evenFrame;
  bool frameOverscan; // if we are overscanning this frame (determined at 0,225)
  bool frameInterlace; // if we are interlacing this frame (determined at start vblank)
  // latching
  uint16_t hCount;
  uint16_t vCount;
  bool hCountSecond;
  bool vCountSecond;
  bool countersLatched;
  // pixel buffer (xbgr)
  // times 2 for even and odd frame

  uint8_t extraLeftCur, extraRightCur, extraLeftRight, extraBottomCur;
  // Widescreen BG3 HUD split (see PpuSetWidescreenHudSplit). 0 height = off.
  uint8_t wsHudSplitHeight, wsHudLeftEnd, wsHudRightStart;
  // Widescreen HUD OAM anchor (see PpuSetWsHudOamShiftRange): an OAM slot
  // range near either native screen edge can move outward with the live
  // margins. Its vertical band can be decoupled from the BG3 split for games
  // whose HUD is sprite-only while BG3 carries dialogue. 0 slots/height = off.
  // A second, independently published range exists for HUD elements the game
  // allocates OUTSIDE its fixed HUD reserve (e.g. MMX boss health bars are
  // general-pool objects at OAM 16+); both ranges share the band/thresholds.
  uint8_t wsHudOamFirstSlot, wsHudOamSlots, wsHudOamHeight;
  uint8_t wsHudOamFirstSlot2, wsHudOamSlots2;
  // Widescreen BG3 widen (see PpuSetWidescreenBg3Widen). Scanlines >= this let
  // BG3 (layer 2) extend into the side margins like BG1/BG2 instead of staying
  // clamped to the authentic 256-wide region. 0 = off (BG3 clamped everywhere,
  // so a BG3 status bar never tiles into the margins). SMW sets it to the HUD
  // band height so water/level content on BG3 below the bar fills 16:9.
  uint8_t wsBg3WidenY;
  // Optional bitmask of BG layers allowed to render in the side margins.
  // Zero preserves the default behavior (BG1/BG2 wide, BG3 policy above).
  uint8_t wsLayerWidenMask;
  // Optional native-width Mode 2 layer capture. Stored as layer+1 so zero is
  // disabled; pixels are palette indices for the most recently drawn frame.
  uint8_t wsMode2CaptureLayer;
  uint8_t wsMode2Capture[224][kPpuXPixels];
  uint8_t wsMode2Bg1Palette[224][kPpuXPixels];
  // Per-layer widescreen policies. Bit L refers to BG(L+1), layer 0..3.
  // Clamp keeps a layer in the authentic 256 columns. Mirror/repeat render
  // the authentic scanline in isolation and use it to fill the side margins.
  uint8_t wsLayerClamp, wsLayerMirror, wsLayerRepeat;
  // Optional scanline bands: clamp or cyclically repeat only [y0,y1).
  uint8_t wsClampY0[4], wsClampY1[4];
  uint8_t wsRepeatY0[4], wsRepeatY1[4];
  // Optional scanline bands: scale a native-width layer across the full
  // widescreen budget. Used for full-screen liquid/effect planes.
  uint8_t wsStretchY0[4], wsStretchY1[4];
  // Optional scanline bands: anchor a layer's left/right chunks to the
  // expanded viewport edges while retaining its middle chunk at native x.
  uint8_t wsAnchorY0[kPpuWsAnchorBands][4];
  uint8_t wsAnchorY1[kPpuWsAnchorBands][4];
  uint8_t wsAnchorLeftEnd[kPpuWsAnchorBands][4];
  uint8_t wsAnchorRightStart[kPpuWsAnchorBands][4];
  // Skip offscreen staging columns before sampling a layer's side margins.
  uint8_t wsMarginGapL[4], wsMarginGapR[4];
  // Treat [left_px, 256-right_px) as a layer's authentic visible viewport
  // when a host line enhancer supplies the expanded scene. This lets the
  // layer's normally hidden/blank native edge columns reveal lower layers.
  uint8_t wsViewportInsetL[4], wsViewportInsetR[4];
  // Presentation-only expansion of game-authored window edges into the side
  // margins. Layer bits use the PPU window layer numbering (BG1..BG4, OBJ,
  // color); window bits select W1/W2. Zero is the hardware-authentic default.
  uint8_t wsWindowExpandLayers, wsWindowExpandWindows;
  // Strict decode of ambiguous 9-bit OAM X margin bands. 1 bit per OAM slot.
  // Left: sprites fully clipped by the authentic 256-wide viewport may become
  // visible in [-extraLeftCur, 0). With strict left hints enabled, only marked
  // slots render there; unmarked slots stay hardware-hidden.
  // Right: raw X values in [256, 256+extraRightCur) are either genuine
  // right-margin sprites or off-screen-left parked sprites wrapped through
  // 9-bit OAM X. With strict right hints enabled, only marked slots keep the
  // positive decode; unmarked slots wrap negative like hardware.
  // Default off preserves the legacy margin behavior. Games publish hints per
  // NMI after CPU-side OAM staging is final and before the frame is presented.
  // A NULL hint pointer disables strict mode. Pass a zeroed array for strict
  // mode with no slots marked.
  uint8_t wsOamLeftHintStrict;
  uint8_t wsOamLeftHint[16];
  uint8_t wsOamRightHintStrict;
  uint8_t wsOamRightHint[16];
  uint8_t lastMosaicModulo;
  uint8_t lastBrightnessMult;
  bool lineHasSprites;
  // Active kPpuRenderFlags_* set by PpuBeginDrawing. Kept outside the stable
  // snapshot region because it is host rendering policy, not emulated state.
  uint32_t renderFlags;
  PpuPixelPrioBufs bgBuffers[2];
  PpuPixelPrioBufs objBuffer;
  /* Per-source isolated priority pixels for generic host-overlay captures. */
  PpuPixelPrioBufs overlayBuffers[kPpuOverlaySource_Count];
  PpuOverlayCapture overlayCaptures[kPpuOverlaySource_Count];
  uint32_t renderPitch;
  uint8_t *renderBuffer;
  uint32_t overlayRenderPitch[kPpuOverlaySource_Count];
  uint8_t *overlayRenderBuffer[kPpuOverlaySource_Count];
  uint8_t brightnessMult[32 + 31];
  uint8_t brightnessMultHalf[32 * 2];
  uint8_t mosaicModulo[kPpuXPixels];

  // Host-only widescreen state; excluded from savestates and cleared by reset.
  PpuWidescreenLineEnhancer *widescreenLineEnhancer;
  void *widescreenLineEnhancerContext;

  // -- START OF SNAPSHOT, 0x10420 bytes
  uint16_t cgram[0x100];
  uint16_t oam[0x100];
  uint8_t highOam[0x20];
  uint16_t vram[0x8000];
  // -- END OF SNAPSHOT


};

#define SPRITE_PRIO_TO_PRIO(prio, level6) (((prio) * 4 + 2) * 16 + 4 + (level6 ? 2 : 0))
#define SPRITE_PRIO_TO_PRIO_HI(prio) ((prio) * 4 + 2)

// Host-only debug render filter (SNESRECOMP_LAYER_MASK env; ppu.c). Guest
// state and savestates are untouched — this only gates final composition.
extern uint8_t g_snes_ppu_dbg_layer_mask;
#define IS_SCREEN_ENABLED(ppu, sub, layer) \
  (ppu->screenEnabled[sub] & g_snes_ppu_dbg_layer_mask & (1 << layer))
#define IS_SCREEN_WINDOWED(ppu, sub, layer) (ppu->screenWindowed[sub] & (1 << layer))
#define GET_WINDOW_FLAGS(ppu, layer) (ppu->windowsel >> (layer * 4))

#define PPU_brightness(ppu) (ppu->inidisp & 0xf)
#define PPU_forcedBlank(ppu) (ppu->inidisp & 0x80)

#define PPU_objSize(ppu) (ppu->obsel >> 5)
#define PPU_objTileAdr1(ppu) ((ppu->obsel & 7) << 13)
#define PPU_objTileAdr2(ppu) (PPU_objTileAdr1(ppu) + (((ppu->obsel & 0x18) + 8) << 9))

#define PPU_objPriority(ppu) (ppu->oamaddh & 0x80)

#define PPU_mode(ppu) (ppu->bgmode & 7)
#define PPU_bg3priority(ppu) (ppu->bgmode & 0x8)
#define PPU_bigTiles(ppu, layer) (ppu->bgmode >> layer & 0x10)

#define PPU_mosaicEnabled(ppu, layer) (ppu->mosaic & (1 << layer))
#define PPU_mosaicSize(ppu) ((ppu->mosaic >> 4) + 1)

#define PPU_bgTilemapWider(ppu, layer) (ppu->bgXsc[layer] & 0x1)
#define PPU_bgTilemapHigher(ppu, layer) (ppu->bgXsc[layer] & 0x2)
#define PPU_bgTilemapAdr(ppu, layer) ((ppu->bgXsc[layer] & 0xfc) << 8)
#define PPU_bgTileAdr(ppu, layer) ((ppu->bgTileAdr >> (layer * 4) & 0xf) << 12)

#define PPU_m7xFlip(ppu) (ppu->m7sel & 0x1)
#define PPU_m7yFlip(ppu) (ppu->m7sel & 0x2)
#define PPU_m7charFill(ppu) (ppu->m7sel & 0x40)
#define PPU_m7largeField(ppu) (ppu->m7sel & 0x80)

#define PPU_directColor(ppu) ((ppu->cgwsel & 0x1) != 0)
#define PPU_addSubscreen(ppu) ((ppu->cgwsel & 0x2) != 0)
#define PPU_preventMathMode(ppu) (ppu->cgwsel >> 4 & 0x3)
#define PPU_clipMode(ppu) (ppu->cgwsel >> 6 & 0x3)

#define PPU_mathEnabled(ppu) (ppu->cgadsub & 0x3f)
#define PPU_halfColor(ppu) ((ppu->cgadsub & 0x40) != 0)
#define PPU_subtractColor(ppu) ((ppu->cgadsub & 0x80) != 0)

#define PPU_fixedColorR(ppu) (ppu->fixedColor & 0x1f)
#define PPU_fixedColorG(ppu) (ppu->fixedColor >> 5 & 0x1f)
#define PPU_fixedColorB(ppu) (ppu->fixedColor >> 10 & 0x1f)

#define PPU_interlace(ppu) ((ppu->setini & 0x1) != 0)
#define PPU_objInterlace(ppu) ((ppu->setini & 0x2) != 0)
#define PPU_overscan(ppu) ((ppu->setini & 0x4) != 0)
#define PPU_pseudoHires(ppu) ((ppu->setini & 0x8) != 0)
#define PPU_m7extBg(ppu) ((ppu->setini & 0x40) != 0)

static inline bool PpuWidescreenLayerRepeatBandActive(
    const Ppu *ppu, unsigned int layer, int y) {
  return layer < 4 &&
      ppu->wsRepeatY1[layer] > ppu->wsRepeatY0[layer] &&
      y >= ppu->wsRepeatY0[layer] && y < ppu->wsRepeatY1[layer];
}

static inline bool PpuWidescreenLayerStretchBandActive(
    const Ppu *ppu, unsigned int layer, int y) {
  return layer < 4 &&
      ppu->wsStretchY1[layer] > ppu->wsStretchY0[layer] &&
      y >= ppu->wsStretchY0[layer] && y < ppu->wsStretchY1[layer];
}

static inline bool PpuWidescreenLineRepeatBandActive(const Ppu *ppu, int y) {
  for (unsigned int layer = 0; layer < 4; layer++) {
    if (PpuWidescreenLayerRepeatBandActive(ppu, layer, y) ||
        PpuWidescreenLayerStretchBandActive(ppu, layer, y))
      return true;
  }
  return false;
}

static inline int PpuWidescreenLayerExtra(
    const Ppu *ppu, unsigned int layer, int y, int extra) {
  if (layer < 4) {
    if (ppu->wsLayerWidenMask &&
        !(ppu->wsLayerWidenMask & (1u << layer)))
      return 0;
    if ((ppu->wsLayerClamp | ppu->wsLayerMirror | ppu->wsLayerRepeat) &
        (1u << layer))
      return 0;
    if (ppu->wsClampY1[layer] > ppu->wsClampY0[layer] &&
        y >= ppu->wsClampY0[layer] && y < ppu->wsClampY1[layer])
      return 0;
    if (PpuWidescreenLayerRepeatBandActive(ppu, layer, y) ||
        PpuWidescreenLayerStretchBandActive(ppu, layer, y))
      return 0;
  }
  if (layer != 2)
    return extra;
  return (ppu->wsBg3WidenY && y >= ppu->wsBg3WidenY) ? extra : 0;
}


enum {
  kWindow1Inversed = 1,
  kWindow1Enabled = 2,
  kWindow2Inversed = 4,
  kWindow2Enabled = 8,
};


Ppu* ppu_init(void);
void ppu_free(Ppu* ppu);
void ppu_reset(Ppu* ppu);
bool ppu_checkOverscan(Ppu* ppu);
void ppu_handleVblank(Ppu* ppu);
void ppu_runLine(Ppu* ppu, int line);
uint8_t ppu_read(Ppu* ppu, uint8_t adr);
void ppu_write(Ppu* ppu, uint8_t adr, uint8_t val);

/* Raster journal — per-line replay of mid-frame INIDISP writes for frame-model
 * hosts. See the block comment in ppu.c. Host calls Begin after its
 * vblank-edge work and ApplyLine in its render loop; the register write path
 * calls Record with the beam line. */
void ppu_rasterBegin(Ppu *ppu);
void ppu_rasterRenderBegin(Ppu *ppu);
int  ppu_rasterTakeHdmaen(uint8_t *out);
void ppu_rasterRecord(uint16_t reg, uint16_t line, uint8_t val);
void ppu_rasterApplyLine(Ppu *ppu, int line);
int  ppu_rasterDebugDump(char *out, int cap);
void ppu_saveload(Ppu *ppu, SaveLoadInfo *sli);
void PpuBeginDrawing(Ppu *ppu, uint8_t *pixels, size_t pitch, uint32_t render_flags);

// Replace stale BG1 tilemap pixels in widened side margins before final
// composition. The callback is host-only and runs independently for main and
// subscreen buffers.
void PpuSetWidescreenLineEnhancer(Ppu *ppu,
                                  PpuWidescreenLineEnhancer *enhancer,
                                  void *context);

// Renderer-neutral host-overlay extraction (opt-in; see
// docs/HOST_OVERLAY_EXTRACTION.md). Default behavior is unchanged: with no
// surface bound and no capture rectangle set, every source is a deterministic
// no-op and the layer stays composited into renderBuffer exactly as before.

// Clear/bind persistent transparent ARGB host-overlay surfaces. Bindings survive
// ppu_reset; capture rectangles do not and are configured by game policy each
// frame. Surfaces are 256-kPpuBufWidth pixels wide and use the same full-frame
// coordinate system as renderBuffer.
// Passing NULL disables extraction for that source. Call ClearBindings once
// after PPU creation so a frontend can explicitly own all optional surfaces.
void PpuClearOverlayBindings(Ppu *ppu);
bool PpuBindOverlaySurface(Ppu *ppu, PpuOverlaySource source,
                           uint8_t *pixels, size_t pitch);

// Clear per-frame capture policy, then configure an arbitrary screen-space
// rectangle from BG1-BG4 or OBJ. With RemoveFromGame, pixels inside the rect
// are omitted from both main and subscreen while still exported with palette,
// transparency, windows, mosaic, and master brightness resolved. Coverage:
// Mode 1 BG1/BG2 (4bpp) and BG3 (2bpp), and Mode 7 BG1; other modes/layers
// leave the source inactive.
void PpuClearOverlayCaptures(Ppu *ppu);
bool PpuSetOverlayCapture(Ppu *ppu, PpuOverlaySource source,
                          int x, int y, int width, int height, uint8_t flags);

// Select a contiguous OAM slot range for an already configured OBJ capture.
// The game remains responsible for validating what those slots represent.
bool PpuSetOverlayOamRange(Ppu *ppu, uint8_t first, uint8_t count);

// Set the symmetric widescreen border, in pixels per side (clamped to
// kPpuExtraLeftRight). 0 restores authentic 256-wide rendering. The internal
// render width becomes 256 + 2*extra. Drives the dormant extraLeftCur/
// extraRightCur/extraLeftRight machinery used by the line renderer.
void PpuSetExtraSpace(Ppu *ppu, uint8_t extra);

// Render authentic 256-wide content centered within a `budget`-per-side wider
// framebuffer (no border columns drawn). For bounded screens; caller blacks
// out the side margins to pillarbox.
void PpuSetExtraSpaceCentered(Ppu *ppu, uint8_t budget);

// Asymmetric per-side widescreen margin (the snesrev/zelda3 model, see
// attribution in IMPROVEMENTS.md). The centering budget (extraLeftRight) must
// already be set via PpuSetExtraSpaceCentered/PpuSetExtraSpace; this fills the
// per-frame extraLeftCur/extraRightCur/extraBottomCur within that budget,
// clamped so the window/sprite/composite paths never read past the
// priority-buffer capacity (left/right) or the 16px overscan bottom. Negative
// inputs clamp to 0. (0,0,0) collapses to a centered pillarbox. Callers
// re-apply per frame (ppu_reset zeroes the fields). Used by games whose own
// scroll/room-bounds state drives the visible margin dynamically (Zelda),
// versus PpuSetExtraSpace's fixed symmetric border (SMW).
void PpuSetExtraSideSpace(Ppu *ppu, int left, int right, int bottom);

// Widescreen HUD split (opt-in, configured by the game frontend): for
// scanlines < height, BG3 (layer 2) is drawn as three chunks — source
// [0,left_end) anchored to the LEFT border edge, [left_end,right_start)
// kept centered (unmoved), [right_start,256) anchored to the RIGHT border
// edge. The vacated spans stay transparent. height 0 = off (authentic).
// Uses the fixed centering budget, independent of dynamic room-bound playfield
// margins, and takes effect while BG3 is not windowed; mosaic lines fall back
// to centered. Like the extra-space
// setters, callers re-apply per frame (ppu_reset zeroes the fields).
void PpuSetWidescreenHudSplit(Ppu *ppu, uint8_t height, uint8_t left_end,
                              uint8_t right_start);

// Opt the split HUD, configured layer-anchor bands, and selected HUD OAM into
// full-budget composition. Pixels outside the live world margin remain black
// unless one of those HUD sources supplied an actual pixel. A BG3 hardware
// window is ignored only inside the configured split-HUD scanline band.
void PpuSetWidescreenHudAlwaysVisible(Ppu *ppu, bool enabled);

// Configure the vertical band and native left/right anchor thresholds used by
// the OAM HUD shifter without enabling the BG3 HUD split. Height may cover the
// full 224-line picture. Existing callers of PpuSetWidescreenHudSplit keep the
// coupled behavior until this overrides it.
void PpuSetWsHudOamBand(Ppu *ppu, uint8_t height, uint8_t left_end,
                        uint8_t right_start);

// Shift edge-hugging HUD sprites in OAM slots [0, nslots) outward with the
// live widescreen margins. Presentation-only; 0 disables the anchor.
/* Deterministic widescreen geometry for probes/CI: SNESRECOMP_WS_EXTRA=<px>
 * pins the per-side margin regardless of window size (0 forces 4:3; unset
 * returns -1 = follow the window). Games consult this when computing their
 * widescreen frame width so measured margins never silently depend on
 * window-manager geometry. Clamped to kPpuExtraLeftRight. */
int PpuWsExtraOverride(void);

void PpuSetWsHudOamShift(Ppu *ppu, uint8_t nslots);

// Shift edge-hugging HUD sprites in OAM slots [first_slot, first_slot+nslots)
// outward with the live widescreen margins. When HUD-always-visible is set,
// use the full centering budget so HUDs can occupy room-bound pillarboxes.
// Presentation-only.
void PpuSetWsHudOamShiftRange(Ppu *ppu, uint8_t first_slot, uint8_t nslots);
/* Second anchored slot range (same band/edge thresholds as the first).
 * For sprite HUDs whose elements live outside the game's fixed HUD OAM
 * reserve. nslots 0 disables. Publish every frame like the first range. */
void PpuSetWsHudOamShiftRange2(Ppu *ppu, uint8_t first_slot, uint8_t nslots);

// Publish this frame's OAM left-margin hints (see wsOamLeftHintStrict).
// `hints` is a 128-bit set (16 bytes, bit N of byte N/8 = OAM slot N), or
// NULL to disable strict decode and restore the legacy left-margin behavior.
// Pass a zeroed array for strict mode with no left-margin slots marked.
void PpuWsSetOamLeftHints(Ppu *ppu, const uint8_t *hints);

// Publish this frame's OAM right-margin hints (see wsOamRightHintStrict).
// `hints` is a 128-bit set (16 bytes, bit N of byte N/8 = OAM slot N), or
// NULL to disable strict decode and restore the legacy always-positive band.
// Pass a zeroed array for strict mode with no right-margin slots marked.
// Games that stage OAM CPU-side should call this each NMI, after the staging
// buffer is final and before the frame is presented.
void PpuWsSetOamRightHints(Ppu *ppu, const uint8_t *hints);

// Let BG3 (layer 2) render into the widescreen side margins on scanlines
// >= from_y, instead of being clamped to the authentic 256-wide region. Pass
// the HUD band height so the status bar above it stays clamped (or split) while
// level content on BG3 below it (e.g. SMW water) fills 16:9. from_y 0 = off.
// Like the other widescreen setters, callers re-apply per frame.
void PpuSetWidescreenBg3Widen(Ppu *ppu, uint8_t from_y);

// Restrict side-margin background rendering to the selected BG layer bits.
// The authentic center and OBJ rendering are unaffected. A zero mask restores
// the default layer policy.
void PpuSetWidescreenLayerMask(Ppu *ppu, uint8_t bg_layer_mask);

// Capture one native 256-pixel Mode 2 background layer before it is combined
// with the other layers. This lets a game frontend synthesize presentation-
// only margins without evaluating offset-per-tile data outside the real SNES
// viewport. Pass layer 0 or 1; any other value disables capture.
void PpuSetMode2LayerCapture(Ppu *ppu, int layer);
const uint8_t *PpuGetMode2LayerCapture(const Ppu *ppu);
const uint8_t *PpuGetMode2Bg1Palette(const Ppu *ppu);

// Per-layer widescreen clamp: bit L keeps BG(L+1) in the authentic 256
// columns while other layers extend into the margins. Re-apply per frame.
// Requires PpuBeginDrawing(..., kPpuRenderFlags_NewRenderer); the legacy
// renderer stores this policy but does not apply it.
void PpuSetWidescreenLayerClamp(Ppu *ppu, uint8_t mask);

// Extend selected game-authored PPU windows by the current widescreen margins
// while evaluating the selected layers. Emulated window registers are not
// changed. Re-apply per frame; zero masks disable the policy.
void PpuSetWidescreenWindowExpansion(Ppu *ppu, uint8_t layer_mask,
                                     uint8_t window_mask);

// Fill Mode-1 background margins by reflecting or cyclically repeating the
// authentic rendered scanline. Rendering remains layer-, priority-, window-,
// and color-math-correct. Repeat wins if both bits are set. Re-apply per frame.
// Requires kPpuRenderFlags_NewRenderer; the legacy renderer ignores these
// policies.
void PpuSetWidescreenLayerMirror(Ppu *ppu, uint8_t mask);
void PpuSetWidescreenLayerRepeat(Ppu *ppu, uint8_t mask);

// Apply clamp, cyclic-repeat, or stretch only on scanlines [y0,y1).
// y1<=y0 disables. Repeat/stretch bands apply to Mode-1 4bpp and 2bpp
// background paths. Requires kPpuRenderFlags_NewRenderer.
void PpuSetWidescreenLayerClampBand(Ppu *ppu, uint8_t layer, uint8_t y0,
                                    uint8_t y1);
void PpuSetWidescreenLayerRepeatBand(Ppu *ppu, uint8_t layer, uint8_t y0,
                                     uint8_t y1);
void PpuSetWidescreenLayerStretchBand(Ppu *ppu, uint8_t layer, uint8_t y0,
                                      uint8_t y1);

// On scanlines [y0,y1), anchor [0,left_end) and [right_start,256) of
// BG(layer+1) to the expanded viewport edges while keeping the middle
// centered. Intended for HUDs embedded in a 4bpp background layer.
void PpuSetWidescreenLayerAnchorBand(Ppu *ppu, uint8_t layer, uint8_t y0,
                                     uint8_t y1, uint8_t left_end,
                                     uint8_t right_start);
// Configure one of the two independent bands available to split-screen
// layouts. Slot 0 is the same band set by the convenience function above.
void PpuSetWidescreenLayerAnchorBandSlot(
    Ppu *ppu, uint8_t slot, uint8_t layer, uint8_t y0, uint8_t y1,
    uint8_t left_end, uint8_t right_start);

// Skip left_px/right_px offscreen tilemap pixels before sampling the margins
// of BG(layer+1). This hides UI staging columns that hardware never displays.
// Applies to non-mosaic 4bpp/2bpp paths. Re-apply per frame.
void PpuSetWidescreenLayerMarginGap(Ppu *ppu, uint8_t layer, uint8_t left_px,
                                    uint8_t right_px);

// Treat [left_px, 256-right_px) as BG(layer+1)'s authentic visible viewport
// while a widescreen line enhancer is active. Pixels in the native edge
// insets remain transparent so the enhancer or lower layers can replace
// framebuffer padding that was hidden on the original display.
void PpuSetWidescreenLayerViewportInset(Ppu *ppu, uint8_t layer,
                                        uint8_t left_px, uint8_t right_px);

#endif
