#ifndef SNESRECOMP_WS_SHADOW_H
#define SNESRECOMP_WS_SHADOW_H

#include <stdbool.h>
#include <stdint.h>

// Presentation-only, world-keyed BG tilemap storage for streaming games.
// Games opt a layer in each frame; the authentic 256-pixel region always
// continues to read real VRAM.
enum {
  kWsShadowXTiles = 4096,
  kWsShadowYTiles = 512,
};

void WsShadowReset(void);
void WsShadowSetWorld(int layer, uint32_t worldX, uint32_t worldY);
void WsShadowSetBlankTile(int layer, int blankEntry);

// Periodic-fold mode: for layers whose content is horizontally periodic
// (typical parallax backdrops), margin tiles are folded to the congruent
// column inside the native 32-column window. The period is re-detected
// from the natively displayed columns every frame, so folded margins can
// never be stale and never expose unwritten map regions; rows with no
// exact period keep the plain map-wrap fallback. Mutually exclusive with
// WsShadowSetWorld for the same layer (the last registration wins).
void WsShadowSetPeriodicFold(int layer);

// Supply an exact raw tilemap entry for a world tile. This is useful when a
// game retains full room data in WRAM but streams only the native viewport to
// VRAM. It changes renderer-side state only.
void WsShadowPrefillTile(int layer, uint32_t worldTileX, uint32_t worldTileY,
                         uint16_t entry);

// Capture the known-good native viewport for later margin use.
struct Ppu;
void WsShadowFrame(const struct Ppu *ppu);

// mapWordAdr = the VRAM word address the renderer fetched realTile from
// (used by fold mode to recover the exact map row/column, independent of
// scroll bias and window splits). hScroll = the layer's live per-line
// scroll: parallax strips change it mid-frame, so fold anchoring must
// use the value the renderer used for THIS line, never a frame sample.
uint16_t WsShadowTile(int layer, int screenX, uint32_t wrappedY,
                      uint16_t hScroll, uint16_t mapWordAdr,
                      uint16_t realTile);
bool WsShadowLayerActive(int layer);

#endif
