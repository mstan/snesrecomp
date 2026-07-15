#include "ws_shadow.h"

#include <stdlib.h>
#include <string.h>

#include "ppu.h"

enum { kLayers = 2 };

typedef struct WsShadowLayer {
  bool registered;
  bool active;
  bool wide;
  uint32_t worldX;
  uint32_t worldY;
  uint16_t mapBaseWord;
  uint16_t *entries;
  uint8_t *valid;
  uint32_t validCount;
  int blankTilePlus1;
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
  }
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
    if (!layer->active || !layer->entries)
      continue;

    layer->mapBaseWord = (uint16_t)PPU_bgTilemapAdr(ppu, i);
    layer->wide = PPU_bgTilemapWider(ppu, i) != 0;
    if (!layer->wide)
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

uint16_t WsShadowTile(int layerIndex, int screenX, uint32_t wrappedY,
                      uint16_t realTile) {
  if (layerIndex < 0 || layerIndex >= kLayers)
    return realTile;
  const WsShadowLayer *layer = &s_layers[layerIndex];
  if (!layer->active || !layer->entries || screenX >= 0 && screenX < 256)
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
         s_layers[layerIndex].entries;
}
