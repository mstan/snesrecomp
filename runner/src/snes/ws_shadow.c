#include "ws_shadow.h"

#include <stdlib.h>
#include <string.h>

#include "ppu.h"

enum { kLayers = 2 };

typedef struct WsShadowLayer {
  bool registered;
  bool active;
  bool wide;
  bool fold;      /* periodic fold enabled (composes with world history) */
  bool worldSet;  /* WsShadowSetWorld called this frame */
  uint32_t worldX;
  uint32_t worldY;
  uint16_t mapBaseWord;
  uint16_t *entries;
  uint8_t *valid;
  uint32_t validCount;
  int blankTilePlus1;
  /* Fold mode: reads the live map at render time. Parallax strips
   * rewrite the layer's scroll mid-frame, so the fold anchor (the
   * leftmost native column) is derived from the per-line hScroll the
   * renderer used, never from a frame sample; the per-row period cache
   * is keyed by that anchor. period 0 = no exact period found -> that
   * (row, anchor) keeps the plain map-wrap fallback. */
  const uint16_t *foldVram;
  struct {
    uint8_t set;
    uint8_t natCol;
    uint8_t period;
  } foldRow[32];
} WsShadowLayer;

static WsShadowLayer s_layers[kLayers];

static bool InBounds(uint32_t tx, uint32_t ty) {
  return tx < kWsShadowXTiles && ty < kWsShadowYTiles;
}

static void SetEntry(WsShadowLayer *layer, uint32_t tx, uint32_t ty,
                     uint16_t entry) {
  if (!layer->entries || !InBounds(tx, ty))
    return;
  uint32_t i = ty * kWsShadowXTiles + tx;
  layer->entries[i] = entry;
  layer->valid[i >> 3] |= (uint8_t)(1u << (i & 7));
}

static bool GetEntry(const WsShadowLayer *layer, uint32_t tx, uint32_t ty,
                     uint16_t *entry) {
  if (!layer->entries || !InBounds(tx, ty))
    return false;
  uint32_t i = ty * kWsShadowXTiles + tx;
  if (!(layer->valid[i >> 3] & (1u << (i & 7))))
    return false;
  *entry = layer->entries[i];
  return true;
}

static int32_t WorldFromWrapped(uint32_t anchor, uint32_t coord) {
  int32_t delta = (int32_t)((coord - anchor) & 0x3ff);
  if (delta >= 512)
    delta -= 1024;
  return (int32_t)anchor + delta;
}

void WsShadowReset(void) {
  for (int i = 0; i < kLayers; i++) {
    WsShadowLayer *layer = &s_layers[i];
    if (layer->valid)
      memset(layer->valid, 0, kWsShadowXTiles * kWsShadowYTiles / 8);
    layer->validCount = 0;
    layer->registered = false;
    layer->active = false;
    layer->fold = false;
    layer->worldSet = false;
    memset(layer->foldRow, 0, sizeof(layer->foldRow));
  }
}

void WsShadowSetPeriodicFold(int layerIndex) {
  if (layerIndex < 0 || layerIndex >= kLayers)
    return;
  WsShadowLayer *layer = &s_layers[layerIndex];
  layer->registered = true;
  layer->fold = true;
  /* Composable with WsShadowSetWorld: rows with a detected period fold
   * to fresh native columns; the remaining (world-anchored) rows fall
   * through to the world-keyed history, then to the plain map wrap. */
}

void WsShadowSetWorld(int layerIndex, uint32_t worldX, uint32_t worldY) {
  if (layerIndex < 0 || layerIndex >= kLayers)
    return;
  WsShadowLayer *layer = &s_layers[layerIndex];
  if (!layer->entries) {
    size_t count = (size_t)kWsShadowXTiles * kWsShadowYTiles;
    layer->entries = (uint16_t *)calloc(count, sizeof(uint16_t));
    layer->valid = (uint8_t *)calloc(count / 8, 1);
    if (!layer->entries || !layer->valid) {
      free(layer->entries);
      free(layer->valid);
      memset(layer, 0, sizeof(*layer));
      return;
    }
  }
  layer->registered = true;
  layer->worldSet = true;
  layer->worldX = worldX;
  layer->worldY = worldY;
}

void WsShadowSetBlankTile(int layerIndex, int blankEntry) {
  if (layerIndex < 0 || layerIndex >= kLayers)
    return;
  s_layers[layerIndex].blankTilePlus1 =
      blankEntry >= 0 && blankEntry <= 0xffff ? blankEntry + 1 : 0;
}

void WsShadowPrefillTile(int layerIndex, uint32_t worldTileX,
                         uint32_t worldTileY, uint16_t entry) {
  if (layerIndex < 0 || layerIndex >= kLayers)
    return;
  WsShadowLayer *layer = &s_layers[layerIndex];
  if (layer->active)
    SetEntry(layer, worldTileX, worldTileY, entry);
}

void WsShadowFrame(const struct Ppu *ppu) {
  for (int i = 0; i < kLayers; i++) {
    WsShadowLayer *layer = &s_layers[i];
    layer->active = layer->registered;
    layer->registered = false;
    if (!layer->active)
      continue;

    layer->mapBaseWord = (uint16_t)PPU_bgTilemapAdr(ppu, i);
    layer->wide = PPU_bgTilemapWider(ppu, i) != 0;
    if (!layer->wide)
      continue;

    if (layer->fold) {
      layer->foldVram = ppu->vram;
      memset(layer->foldRow, 0, sizeof(layer->foldRow));
    }
    bool sweep = layer->worldSet && layer->entries;
    layer->worldSet = false;
    if (!sweep)
      continue;

    uint32_t tx0 = layer->worldX >> 3;
    uint32_t ty0 = layer->worldY >> 3;
    for (int col = 0; col < 32; col++) {
      uint32_t tx = tx0 + (uint32_t)col;
      int mapCol = (int)(tx & 63);
      int half = mapCol >= 32;
      for (int row = 0; row < 29; row++) {
        uint32_t ty = ty0 + (uint32_t)row;
        int mapRow = (int)(ty & 31);
        uint16_t word = (uint16_t)(layer->mapBaseWord +
            (half ? 0x400 : 0) + (mapRow << 5) + (mapCol & 31));
        SetEntry(layer, tx, ty, ppu->vram[word & 0x7fff]);
      }
    }
  }
}

static uint16_t FoldMapEntry(const WsShadowLayer *layer, int row, int col) {
  uint16_t word = (uint16_t)(layer->mapBaseWord +
      (col >= 32 ? 0x400 : 0) + (row << 5) + (col & 31));
  return layer->foldVram[word & 0x7fff];
}

/* Detect the row's horizontal period over the native 32-column window
 * anchored at natCol. Only natively displayed columns are trusted: they
 * are correct-by-definition on this very line, so a fold anchored there
 * can never serve stale or unwritten map content. Periods must divide 64
 * so the renderer's mod-64 column wrap preserves congruence. */
static uint8_t FoldRowPeriod(WsShadowLayer *layer, int row, int natCol) {
  static const uint8_t kPeriods[] = {4, 8, 16};
  if (layer->foldRow[row].set && layer->foldRow[row].natCol == natCol)
    return layer->foldRow[row].period;
  uint8_t period = 0;
  for (size_t p = 0; p < sizeof(kPeriods) && !period; p++) {
    int ok = 1;
    for (int i = 0; i + kPeriods[p] < 32 && ok; i++)
      ok = FoldMapEntry(layer, row, (natCol + i) & 63) ==
           FoldMapEntry(layer, row, (natCol + i + kPeriods[p]) & 63);
    if (ok)
      period = kPeriods[p];
  }
  layer->foldRow[row].set = 1;
  layer->foldRow[row].natCol = (uint8_t)natCol;
  layer->foldRow[row].period = period;
  return period;
}

uint16_t WsShadowTile(int layerIndex, int screenX, uint32_t wrappedY,
                      uint16_t hScroll, uint16_t mapWordAdr,
                      uint16_t realTile) {
  if (layerIndex < 0 || layerIndex >= kLayers)
    return realTile;
  WsShadowLayer *layer = &s_layers[layerIndex];
  if (!layer->active || screenX >= 0 && screenX < 256)
    return realTile;

  /* Layered margin sources: (1) periodic fold for rows whose native
   * window proves an exact period — always this-frame fresh; (2) the
   * world-keyed history for the remaining (world-anchored) rows;
   * (3) the renderer's plain map wrap as the final fallback. */
  if (layer->fold && layer->foldVram) {
    uint16_t off = (uint16_t)(mapWordAdr - layer->mapBaseWord);
    if (off < 0x800) {
      int col = (off & 0x1f) | (off & 0x400 ? 0x20 : 0);
      int row = (off >> 5) & 0x1f;
      int natCol = (hScroll >> 3) & 63;
      uint8_t period = FoldRowPeriod(layer, row, natCol);
      if (period) {
        int rel = (col - natCol) & 63;
        if (rel < 32)
          return realTile;  /* native column (or margin overlapping it) */
        return FoldMapEntry(layer, row, (natCol + rel % period) & 63);
      }
      /* no exact period: fall through to the world-keyed history */
    }
  }

  if (!layer->entries)
    return realTile;

  uint16_t miss = layer->blankTilePlus1
                      ? (uint16_t)(layer->blankTilePlus1 - 1)
                      : realTile;
  int32_t worldX = (int32_t)layer->worldX + screenX;
  int32_t worldY = WorldFromWrapped(layer->worldY, wrappedY & 0x3ff);
  if (worldX < 0 || worldY < 0)
    return miss;
  uint16_t entry;
  if (GetEntry(layer, (uint32_t)worldX >> 3, (uint32_t)worldY >> 3, &entry))
    return entry;
  return miss;
}

bool WsShadowLayerActive(int layerIndex) {
  return layerIndex >= 0 && layerIndex < kLayers &&
         s_layers[layerIndex].active && s_layers[layerIndex].wide &&
         (s_layers[layerIndex].fold || s_layers[layerIndex].entries);
}
