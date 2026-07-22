#ifndef SNESRECOMP_WS_SHADOW_H
#define SNESRECOMP_WS_SHADOW_H

#include <stdbool.h>
#include <stdint.h>

// Presentation-only, world-keyed BG tilemap storage for streaming games.
// Games opt a layer in each frame; the authentic 256-pixel region always
// continues to read real VRAM. Supports 8x8 and 16x16 (BGMODE big-tile)
// layers, and both 64-wide and streamed 32-wide tilemaps.
enum {
  kWsShadowXTiles = 4096,
  kWsShadowYTiles = 512,
};

void WsShadowReset(void);

/* True world-space camera origin for shadow keys (tile capture / margins).
 * For games where PPU scroll == camera (SMW-style), pass hScroll/vScroll.
 * For strip-streaming games (Metal Warriors), pass the WRAM camera and also
 * call WsShadowSetScroll with the PPU buffer scroll. */
void WsShadowSetWorld(int layer, uint32_t worldX, uint32_t worldY);

/* PPU scroll used to address the streaming VRAM tilemap window and to convert
 * scanline-wrapped Y back to world Y. Defaults to the SetWorld values. */
void WsShadowSetScroll(int layer, uint32_t scrollX, uint32_t scrollY);

void WsShadowSetBlankTile(int layer, int blankEntry);

// Periodic-fold mode: for layers whose content is horizontally periodic
// (typical parallax backdrops), margin tiles are folded to the congruent
// column inside the native 32-column window. The period is re-detected
// from the natively displayed columns every frame, so folded margins can
// never be stale and never expose unwritten map regions; rows with no
// exact period keep the plain map-wrap fallback. Mutually exclusive with
// WsShadowSetWorld for the same layer (the last registration wins).
void WsShadowSetPeriodicFold(int layer);

/* Keep world-keyed entries across frames instead of clearing every present.
 * The viewport capture still overwrites in-view tiles each frame, so only
 * off-view margins read history. History is dropped automatically when the
 * layer's tilemap base changes (room/scene switch). Off by default. */
void WsShadowSetRetainHistory(int layer, bool retain);

/* Total tilemap columns to capture per frame (default: the 256px viewport
 * plus one fine-scroll overhang column). Games that draw extra valid columns
 * beyond the viewport (streaming headroom) can widen this so margins pick
 * them up. Clamped to the 32/64-column map width. */
void WsShadowSetCaptureCols(int layer, int totalCols);

/* How many tile columns west of the live strip retainHistory keeps (default
 * 12 ≈ 192px at 16×16). Widescreen games should set this to the margin
 * budget in tiles (+ small slop) so left-gutter props despawn once they
 * leave the wide viewport instead of lingering for a full native buffer. */
void WsShadowSetWestKeep(int layer, int tiles);

/* When set, capture columns east of the 256px view that match any live
 * view column are cleared instead of stored — kills VRAM-wrap / period
 * phantoms (e.g. a second door) in the right widescreen gutter. */
void WsShadowSetRejectEastEcho(int layer, bool reject);

// Supply an exact raw tilemap entry for a world tile. This is useful when a
// game retains full room data in WRAM but streams only the native viewport to
// VRAM. It changes renderer-side state only. Does not overwrite an entry that
// Frame/history already captured.
void WsShadowPrefillTile(int layer, uint32_t worldTileX, uint32_t worldTileY,
                         uint16_t entry);

/* Like PrefillTile but always writes (DMA-pad VRAM beats $7F guesses). */
void WsShadowForceTile(int layer, uint32_t worldTileX, uint32_t worldTileY,
                       uint16_t entry);

/* retainHistory layers only: force a west-of-view tile at a viewport-row Y
 * key (not world Y). Same ownership model as live capture: every present
 * overwrites. Games call this with strip memory west of the DMA base so the
 * left gutter tracks current source like the 4:3 strip / right headroom.
 * worldTileX must be < tx0. */
void WsShadowForceWestViewportTile(int layer, uint32_t worldTileX,
                                   uint32_t viewportRow, uint16_t entry);

/* Like ForceWestViewportTile but only fills missing cells. Use when the ROM
 * strip base barely moves with the camera — Force every present would drag
 * the same west decoration through world space (sticky left chains). */
void WsShadowPrefillWestViewportTile(int layer, uint32_t worldTileX,
                                     uint32_t viewportRow, uint16_t entry);

/* Drop a world-keyed entry (e.g. reject stale DMA-pad echo columns). */
void WsShadowInvalidateTile(int layer, uint32_t worldTileX,
                            uint32_t worldTileY);

/* After WsShadowFrame: fill still-missing margin columns by repeating the
 * nearest captured viewport-edge column. Used when the game only streams a
 * 256px strip and no CPU-side map buffer is available to prefill from.
 * marginPixels is the per-side widescreen budget (e.g. g_ws_extra). */
void WsShadowExtendEdges(int layer, int marginPixels);

/* Like ExtendEdges, but searches inward for the nearest non-zero tile and
 * fills missing/zero margin cells (and zero overhang) so layers continue
 * into the gutter under transparent holes. */
void WsShadowExtendSolidEdges(int layer, int marginPixels);

/* BG1 seam only: copy the last captured view column into the first missing
 * column past the view (at most one tile each side). No full-gutter smear. */
void WsShadowContinueSeam(int layer);

// Capture the known-good native viewport for later margin use.
struct Ppu;
void WsShadowFrame(const struct Ppu *ppu);

// Feed a VRAM word write (post-merge value) from the emulation's write
// paths. Writes landing inside a registered wide layer's tilemap are
// captured into the world-keyed history, bound to the world chunk the
// upload was staged for (half parity + travel direction). No-op for
// inactive layers/addresses.
void WsShadowOnVramWrite(uint16_t wordAdr, uint16_t value);

// mapWordAdr = the VRAM word address the renderer fetched realTile from
// (used by fold mode to recover the exact map row/column, independent of
// scroll bias and window splits). hScroll = the layer's live per-line
// scroll: parallax strips change it mid-frame, so fold anchoring must
// use the value the renderer used for THIS line, never a frame sample.
uint16_t WsShadowTile(int layer, int screenX, uint32_t wrappedY,
                      uint16_t hScroll, uint16_t mapWordAdr,
                      uint16_t realTile);
bool WsShadowLayerActive(int layer);

/* Latched world/scroll origins for margin pixel-phase (must match tile keys). */
uint32_t WsShadowWorldX(int layer);
uint32_t WsShadowWorldY(int layer);
/* Presentation Y for a margin screenX (west may use a frozen follow origin). */
uint32_t WsShadowPresentWorldY(int layer, int screenX);
uint32_t WsShadowScrollX(int layer);
uint32_t WsShadowScrollY(int layer);

#endif
