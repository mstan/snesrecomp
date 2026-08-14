#include "ws_shadow.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ppu.h"

enum { kLayers = 2 };

enum {
  kWsWestKeep = 12, /* ~192px; gutter + wide platforms */
  kWsLiveMaxCols = 32,
  kWsLiveMaxRows = 32,
  kWsLiveDyMax = 8
};

typedef struct WsShadowLayer {
  bool registered;
  bool active;
  bool wide;
  bool fold;      /* periodic fold enabled (composes with world history) */
  bool worldSet;  /* WsShadowSetWorld called this frame */
  int dir;        /* last nonzero worldX motion: +1 right, -1 left */
  uint8_t tileShift; /* 3 = 8x8 map entries, 4 = 16x16 big tiles */
  uint32_t worldX;
  uint32_t worldY;
  uint32_t scrollX;
  uint32_t scrollY;
  uint16_t mapBaseWord;
  uint16_t *entries;
  uint8_t *valid;
  /* Per-cell origin class: bit set = the entry is a game-map GUESS seeded
   * by WsShadowPrefillTile; bit clear = captured from the game's own VRAM
   * (native sweep / OnVramWrite / ForceTile). Prefill may refresh its own
   * guesses when the game's CPU-side map is still streaming at seed time
   * (e.g. MMX room decompression racing the first widescreen frame);
   * captured entries are authoritative and never yield to a guess. */
  uint8_t *guessOrigin;
  uint32_t validCount;
  int blankTilePlus1;
  uint32_t lastTx0;
  uint32_t lastTy0;
  bool haveLastOrigin;
  bool retainHistory;
  int captureCols;
  int westKeep; /* 0 => kWsWestKeep default */
  int eastKeep; /* 0 => legacy behaviour: east-of-view history is pruned */
  /* ForceTile yields to cells the game itself wrote within this many
   * frames (0 = off). Games that draw moving objects into the BG tilemap
   * (platforms, elevators) need their dynamic writes to win over an exact
   * margin refill, or the object is erased outside the native window. */
  int respectGameWrites;
  bool rejectEastEcho;
  uint16_t retainMapBase;
  bool haveRetainMapBase;
  uint8_t *cooldown;
  uint16_t prevLive[kWsLiveMaxCols * kWsLiveMaxRows];
  uint8_t prevLiveOcc[kWsLiveMaxCols * kWsLiveMaxRows];
  int prevLiveCols;
  int prevLiveRows;
  uint32_t prevLiveTx0;
  uint32_t prevScrollY;
  bool havePrevLive;
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

/* Always-on margin lookup accounting (see WsShadowTile). Never armed. */
static WsShadowMarginStat s_marginStats[kLayers];

static bool GetEntry(const WsShadowLayer *layer, uint32_t tx, uint32_t ty,
                     uint16_t *entry);

void WsShadowGetMarginStats(int layerIndex, WsShadowMarginStat *out) {
  if (!out)
    return;
  if (layerIndex < 0 || layerIndex >= kLayers) {
    WsShadowMarginStat zero = {0};
    *out = zero;
    return;
  }
  *out = s_marginStats[layerIndex];
}

bool WsShadowLookupWorldTile(int layerIndex, uint32_t worldTileX,
                             uint32_t worldTileY, uint16_t *entry) {
  if (!entry || layerIndex < 0 || layerIndex >= kLayers)
    return false;
  return GetEntry(&s_layers[layerIndex], worldTileX, worldTileY, entry);
}

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
  /* Every non-prefill write is a capture (or an exact Force): it wins over
   * any future prefill guess for this cell. */
  if (layer->guessOrigin)
    layer->guessOrigin[i >> 3] &= (uint8_t)~(1u << (i & 7));
}

static void SetEntryGuess(WsShadowLayer *layer, uint32_t tx, uint32_t ty,
                          uint16_t entry) {
  if (!layer->entries || !InBounds(tx, ty))
    return;
  uint32_t i = ty * kWsShadowXTiles + tx;
  layer->entries[i] = entry;
  layer->valid[i >> 3] |= (uint8_t)(1u << (i & 7));
  if (layer->guessOrigin)
    layer->guessOrigin[i >> 3] |= (uint8_t)(1u << (i & 7));
}

static bool IsGuessOrigin(const WsShadowLayer *layer, uint32_t tx,
                          uint32_t ty) {
  if (!layer->guessOrigin || !InBounds(tx, ty))
    return false;
  uint32_t i = ty * kWsShadowXTiles + tx;
  return (layer->guessOrigin[i >> 3] & (1u << (i & 7))) != 0;
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

static void ClearEntry(WsShadowLayer *layer, uint32_t tx, uint32_t ty) {
  if (!layer->valid || !InBounds(tx, ty))
    return;
  uint32_t i = ty * kWsShadowXTiles + tx;
  layer->valid[i >> 3] &= (uint8_t)~(1u << (i & 7));
}

static bool IsLiveOpaque(uint16_t tile) {
  return tile != 0 && tile != 0x0200u && tile != 0x0DAEu;
}

void WsShadowReset(void) {
  for (int i = 0; i < kLayers; i++) {
    WsShadowLayer *layer = &s_layers[i];
    if (layer->valid)
      memset(layer->valid, 0, kWsShadowXTiles * kWsShadowYTiles / 8);
    if (layer->guessOrigin)
      memset(layer->guessOrigin, 0, kWsShadowXTiles * kWsShadowYTiles / 8);
    if (layer->cooldown)
      memset(layer->cooldown, 0, (size_t)kWsShadowXTiles * kWsShadowYTiles);
    layer->validCount = 0;
    layer->registered = false;
    layer->active = false;
    layer->fold = false;
    layer->worldSet = false;
    layer->haveLastOrigin = false;
    layer->haveRetainMapBase = false;
    layer->havePrevLive = false;
    layer->prevLiveCols = 0;
    layer->prevLiveRows = 0;
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
    layer->guessOrigin = (uint8_t *)calloc(count / 8, 1);
    layer->cooldown = (uint8_t *)calloc(count, 1);
    if (!layer->entries || !layer->valid || !layer->guessOrigin ||
        !layer->cooldown) {
      free(layer->entries);
      free(layer->valid);
      free(layer->guessOrigin);
      free(layer->cooldown);
      memset(layer, 0, sizeof(*layer));
      return;
    }
  }
  layer->registered = true;
  layer->worldSet = true;
  if (worldX != layer->worldX)
    layer->dir = ((int32_t)(worldX - layer->worldX) > 0) ? 1 : -1;
  layer->worldX = worldX;
  layer->worldY = worldY;
  layer->scrollX = worldX;
  layer->scrollY = worldY;
}

void WsShadowSetScroll(int layerIndex, uint32_t scrollX, uint32_t scrollY) {
  if (layerIndex < 0 || layerIndex >= kLayers)
    return;
  WsShadowLayer *layer = &s_layers[layerIndex];
  layer->scrollX = scrollX;
  layer->scrollY = scrollY;
}

/* World y-tile for a map row, using the anchor's 32-row wrap window. */
static uint32_t WorldRowForMapRow(const WsShadowLayer *layer, int row) {
  const unsigned sh = layer->tileShift ? layer->tileShift : 3;
  uint32_t wy0 = layer->worldY >> sh;
  return wy0 + (uint32_t)((row - (int)(wy0 & 31)) & 31);
}

/* Capture the game's own tilemap uploads as they land, bound to the
 * world chunk they were staged for. The map holds two 256px chunks with
 * fixed half parity, so a written column's chunk is unambiguous when its
 * half matches the camera chunk's parity; the other half is the chunk
 * being staged ahead (or behind, per the last travel direction). This
 * feeds freshly staged content - including first-visit world-anchored
 * features - into the history the moment it exists in VRAM. */
void WsShadowOnVramWrite(uint16_t wordAdr, uint16_t value) {
  for (int i = 0; i < kLayers; i++) {
    WsShadowLayer *layer = &s_layers[i];
    if (!layer->active || !layer->entries)
      continue;
    uint16_t off = (uint16_t)(wordAdr - layer->mapBaseWord);
    if (off >= (layer->wide ? 0x800 : 0x400))
      continue;
    int col =
        (off & 0x1f) | (layer->wide && (off & 0x400) ? 0x20 : 0);
    int row = (off >> 5) & 0x1f;
    const unsigned sh = layer->tileShift ? layer->tileShift : 3;
    uint32_t k0 = layer->worldX >> (sh + 5);
    uint32_t chunk;
    if (!layer->wide)
      chunk = k0;
    else if ((uint32_t)(col >> 5) == (k0 & 1))
      chunk = k0;
    else
      chunk = layer->dir < 0 ? k0 - 1 : k0 + 1;
    uint32_t tx = chunk * 32 + (uint32_t)(col & 31);
    uint32_t ty = WorldRowForMapRow(layer, row);
    SetEntry(layer, tx, ty, value);
    /* Frame-stamp the game's own write so a respectGameWrites margin
     * refill yields to it (dynamic BG objects: platforms, elevators). */
    if (layer->respectGameWrites && layer->cooldown && InBounds(tx, ty)) {
      extern int snes_frame_counter;
      layer->cooldown[ty * kWsShadowXTiles + tx] =
          (uint8_t)(snes_frame_counter & 0xFF);
    }
  }
}

void WsShadowSetBlankTile(int layerIndex, int blankEntry) {
  if (layerIndex < 0 || layerIndex >= kLayers)
    return;
  s_layers[layerIndex].blankTilePlus1 =
      blankEntry >= 0 && blankEntry <= 0xffff ? blankEntry + 1 : 0;
}

void WsShadowSetRetainHistory(int layerIndex, bool retain) {
  if (layerIndex < 0 || layerIndex >= kLayers)
    return;
  s_layers[layerIndex].retainHistory = retain;
}

void WsShadowSetCaptureCols(int layerIndex, int totalCols) {
  if (layerIndex < 0 || layerIndex >= kLayers)
    return;
  s_layers[layerIndex].captureCols = totalCols;
}

void WsShadowSetWestKeep(int layerIndex, int tiles) {
  if (layerIndex < 0 || layerIndex >= kLayers)
    return;
  if (tiles < 0)
    tiles = 0;
  if (tiles > kWsLiveMaxCols)
    tiles = kWsLiveMaxCols;
  s_layers[layerIndex].westKeep = tiles;
}

void WsShadowSetRejectEastEcho(int layerIndex, bool reject) {
  if (layerIndex < 0 || layerIndex >= kLayers)
    return;
  s_layers[layerIndex].rejectEastEcho = reject;
}

void WsShadowSetEastKeep(int layerIndex, int tiles) {
  if (layerIndex < 0 || layerIndex >= kLayers)
    return;
  if (tiles < 0)
    tiles = 0;
  if (tiles > kWsLiveMaxCols)
    tiles = kWsLiveMaxCols;
  s_layers[layerIndex].eastKeep = tiles;
}

static int WestKeep(const WsShadowLayer *layer) {
  return layer->westKeep > 0 ? layer->westKeep : kWsWestKeep;
}

/* 0 by default, which reproduces the original prune exactly: everything east
 * of the live strip is dropped each frame. That is correct only for a game
 * scrolling right, where east is the LEADING edge and has no history worth
 * keeping. A game scrolling LEFT has its trailing edge in the east, and
 * dropping it means the trailing gutter can never be served from history. */
static int EastKeep(const WsShadowLayer *layer) {
  return layer->eastKeep > 0 ? layer->eastKeep : 0;
}

void WsShadowPrefillTile(int layerIndex, uint32_t worldTileX,
                         uint32_t worldTileY, uint16_t entry) {
  if (layerIndex < 0 || layerIndex >= kLayers)
    return;
  WsShadowLayer *layer = &s_layers[layerIndex];
  if (layer->retainHistory)
    return;
  if (layer->entries && (layer->active || layer->registered)) {
    uint16_t cur;
    if (!GetEntry(layer, worldTileX, worldTileY, &cur)) {
      SetEntryGuess(layer, worldTileX, worldTileY, entry);
      s_marginStats[layerIndex].prefillSeed++;
    } else if (cur != entry && IsGuessOrigin(layer, worldTileX, worldTileY)) {
      /* The game's CPU-side map changed under an earlier guess. This is
       * routine while a room's map is still streaming in (a clean-launch
       * first frame can seed margins from a half-decompressed map); the
       * newest resolve of the same map is strictly fresher, so take it.
       * Captured entries (guess bit clear) never yield: real VRAM content
       * beats any guess, and re-guessing it would fight OnVramWrite. */
      SetEntryGuess(layer, worldTileX, worldTileY, entry);
      s_marginStats[layerIndex].prefillRefresh++;
    }
  }
}

int WsShadowDebugCell(int layerIndex, uint32_t worldTileX, uint32_t worldTileY,
                      uint16_t *entry) {
  if (layerIndex < 0 || layerIndex >= kLayers)
    return 0;
  const WsShadowLayer *layer = &s_layers[layerIndex];
  uint16_t e = 0;
  if (!GetEntry(layer, worldTileX, worldTileY, &e))
    return 0;
  if (entry)
    *entry = e;
  return IsGuessOrigin(layer, worldTileX, worldTileY) ? 2 : 1;
}

void WsShadowSetRespectGameWrites(int layerIndex, int frames) {
  if (layerIndex < 0 || layerIndex >= kLayers)
    return;
  if (frames < 0)
    frames = 0;
  if (frames > 250) /* u8 frame stamps; keep aliasing window sane */
    frames = 250;
  s_layers[layerIndex].respectGameWrites = frames;
}

void WsShadowForceTile(int layerIndex, uint32_t worldTileX,
                       uint32_t worldTileY, uint16_t entry) {
  if (layerIndex < 0 || layerIndex >= kLayers)
    return;
  WsShadowLayer *layer = &s_layers[layerIndex];
  if (layer->retainHistory)
    return;
  if (!layer->entries || !(layer->active || layer->registered))
    return;
  /* Yield to a recent game write (dynamic BG object drawn in the margin
   * through widened object windows). Stamp 0 doubles as "never written";
   * the 1-in-256 frame whose low byte is 0 merely skips one stamp. u8
   * stamps alias every 256 frames, so an ancient write can read fresh
   * for up to `respect` frames — it then serves the game's own captured
   * entry (real content, one-wrap old) rather than anything invented. */
  if (layer->respectGameWrites && layer->cooldown &&
      InBounds(worldTileX, worldTileY)) {
    uint8_t stamp = layer->cooldown[worldTileY * kWsShadowXTiles + worldTileX];
    if (stamp) {
      extern int snes_frame_counter;
      uint8_t age = (uint8_t)((snes_frame_counter & 0xFF) - stamp);
      if (age <= (uint8_t)layer->respectGameWrites)
        return;
    }
  }
  SetEntry(layer, worldTileX, worldTileY, entry);
}

void WsShadowForceWestViewportTile(int layerIndex, uint32_t worldTileX,
                                   uint32_t viewportRow, uint16_t entry) {
  if (layerIndex < 0 || layerIndex >= kLayers)
    return;
  WsShadowLayer *layer = &s_layers[layerIndex];
  if (!layer->retainHistory || !layer->entries ||
      !(layer->active || layer->registered))
    return;
  const unsigned sh = layer->tileShift ? layer->tileShift : 3;
  const uint32_t tx0 = layer->worldX >> sh;
  const int rows = sh == 4 ? 16 : 29;
  if (worldTileX >= tx0 || viewportRow >= (uint32_t)rows)
    return;
  /* Always force — parity with live VRAM SetEntry each present. */
  SetEntry(layer, worldTileX, viewportRow, entry);
}

void WsShadowPrefillWestViewportTile(int layerIndex, uint32_t worldTileX,
                                     uint32_t viewportRow, uint16_t entry) {
  if (layerIndex < 0 || layerIndex >= kLayers)
    return;
  WsShadowLayer *layer = &s_layers[layerIndex];
  if (!layer->retainHistory || !layer->entries ||
      !(layer->active || layer->registered))
    return;
  const unsigned sh = layer->tileShift ? layer->tileShift : 3;
  const uint32_t tx0 = layer->worldX >> sh;
  const int rows = sh == 4 ? 16 : 29;
  if (worldTileX >= tx0 || viewportRow >= (uint32_t)rows)
    return;
  uint16_t ignore;
  if (!GetEntry(layer, worldTileX, viewportRow, &ignore))
    SetEntry(layer, worldTileX, viewportRow, entry);
}

void WsShadowInvalidateTile(int layerIndex, uint32_t worldTileX,
                            uint32_t worldTileY) {
  if (layerIndex < 0 || layerIndex >= kLayers)
    return;
  /* retainHistory allowed: west ROM refresh clears void cells so stale
   * opaque history does not linger beside a moved strip. */
  ClearEntry(&s_layers[layerIndex], worldTileX, worldTileY);
}

/* Fill still-missing margin cells by repeating the nearest captured viewport
 * edge column. Was a no-op stub despite being documented, which silently
 * defeated every caller relying on it for the LEADING gutter.
 *
 * This is the only source available for a leading edge: its world content has
 * never been displayed, so history has nothing, and a 256px streamer with no
 * readable CPU-side map (Mega Man X2) has nothing to prefill from either.
 * Repeating the edge column is an approximation, but a bounded one -- and the
 * alternative is the renderer's plain map wrap, which shows a DIFFERENT
 * SECTION of the level.
 *
 * Only fills cells that are missing, so real history and real captures always
 * win. Call after WsShadowFrame.
 *
 * Row keying matches capture exactly: retainHistory layers key by VIEWPORT row
 * (0..rows-1), everything else by world row (ty0 + row). Getting this wrong
 * writes cells the serve path never reads. */
void WsShadowExtendEdges(int layerIndex, int marginPixels) {
  (void)layerIndex;
  (void)marginPixels;
  /* REVERTED TO A NO-OP 2026-07-26 after owner review.
   *
   * A working implementation existed briefly (repeat the nearest captured
   * viewport-edge column outward into the gutter). It did what it said and
   * drove margin lookup misses from ~90% to 0 -- and it looked DEFINITIVELY
   * WORSE than the stale margin it replaced. Repeating one column produces
   * obvious vertical smearing: long white pipe runs and structural edges
   * stretch into hard vertical streaks across the whole gutter, which reads
   * as a rendering fault rather than as scenery.
   *
   * The lesson for whoever picks this up: "no lookup misses" is not the
   * success metric. A wrong-but-plausible margin beats a smeared one, and a
   * naive edge repeat is not plausible on architectural content with strong
   * horizontal structure. Something that reconstructs actual level geometry
   * (or that hides the gutter) is needed instead.
   *
   * Kept as a stub so the engine's rendering behaviour matches the tagged
   * ws-baseline-prehistory exactly. The implementation is recoverable from
   * engine commit 03b1588 if a future attempt wants it as a starting point. */
}

void WsShadowExtendSolidEdges(int layerIndex, int extraPx) {
  (void)layerIndex;
  (void)extraPx;
}

void WsShadowContinueSeam(int layerIndex) {
  (void)layerIndex;
}

/* Shift west columns in viewport-row space. dy > 0 ⇒ content moves toward
 * lower row indices (up on screen), matching EstimateLiveViewportDy. */
static void ShiftWestViewportRows(WsShadowLayer *layer, uint32_t tx0, int dy,
                                  int rows) {
  if (!layer->entries || dy == 0 || rows <= 0)
    return;
  uint16_t tmp_e[kWsLiveMaxRows];
  uint8_t tmp_v[kWsLiveMaxRows];
  const int row_n = rows < kWsLiveMaxRows ? rows : kWsLiveMaxRows;
  const int west_keep = WestKeep(layer);
  for (int64_t tx = (int64_t)tx0 - west_keep; tx < (int64_t)tx0; tx++) {
    if (tx < 0 || tx >= kWsShadowXTiles)
      continue;
    for (int r = 0; r < row_n; r++) {
      uint16_t e = 0;
      tmp_v[r] = GetEntry(layer, (uint32_t)tx, (uint32_t)r, &e) ? 1 : 0;
      tmp_e[r] = e;
    }
    for (int r = 0; r < row_n; r++)
      ClearEntry(layer, (uint32_t)tx, (uint32_t)r);
    for (int r = 0; r < row_n; r++) {
      const int src = r + dy;
      if (src < 0 || src >= row_n || !tmp_v[src])
        continue;
      SetEntry(layer, (uint32_t)tx, (uint32_t)r, tmp_e[src]);
    }
  }
}

/*
 * After a viewport-row shift, the vacated band has no history — that is the
 * "upper half of the elevator never appears on the left" bug when content
 * moves down (dy < 0) and new VRAM rows enter at the top. Fill vacated
 * cells from the matching live seam column when possible, else extend the
 * west column's own vertical strip (repeating chain tiles).
 */
static void BackfillWestVacatedRows(WsShadowLayer *layer, uint32_t tx0,
                                    const uint16_t *live, const uint8_t *live_occ,
                                    int cols, int rows, int dy) {
  if (!layer->entries || dy == 0 || rows <= 0 || cols <= 0)
    return;
  const int row_n = rows < kWsLiveMaxRows ? rows : kWsLiveMaxRows;
  int r0, r1;
  if (dy < 0) {
    r0 = 0;
    r1 = -dy < row_n ? -dy : row_n;
  } else {
    r0 = row_n - dy;
    if (r0 < 0)
      r0 = 0;
    r1 = row_n;
  }
  if (r0 >= r1)
    return;

  const int west_keep = WestKeep(layer);
  for (int64_t tx = (int64_t)tx0 - west_keep; tx < (int64_t)tx0; tx++) {
    if (tx < 0 || tx >= kWsShadowXTiles)
      continue;
    int any = 0;
    for (int r = 0; r < row_n; r++) {
      uint16_t e = 0;
      if (GetEntry(layer, (uint32_t)tx, (uint32_t)r, &e)) {
        any = 1;
        break;
      }
    }
    if (!any)
      continue;

    int dist = (int)((int64_t)tx0 - tx); /* 1 = seam-adjacent */
    int lc = dist - 1;
    if (lc < 0)
      lc = 0;
    if (lc >= cols)
      lc = cols - 1;

    for (int r = r0; r < r1; r++) {
      uint16_t cur = 0;
      if (GetEntry(layer, (uint32_t)tx, (uint32_t)r, &cur))
        continue;
      const int li = r * kWsLiveMaxCols + lc;
      if (live_occ[li] && IsLiveOpaque(live[li])) {
        SetEntry(layer, (uint32_t)tx, (uint32_t)r, live[li]);
        continue;
      }
      /* Vertical extend within this west column. */
      uint16_t ext = 0;
      int found = 0;
      if (dy < 0) {
        for (int s = r1; s < row_n; s++) {
          if (GetEntry(layer, (uint32_t)tx, (uint32_t)s, &ext)) {
            found = 1;
            break;
          }
        }
      } else {
        for (int s = r0 - 1; s >= 0; s--) {
          if (GetEntry(layer, (uint32_t)tx, (uint32_t)s, &ext)) {
            found = 1;
            break;
          }
        }
      }
      if (found && IsLiveOpaque(ext))
        SetEntry(layer, (uint32_t)tx, (uint32_t)r, ext);
    }
  }
}

/*
 * Live strip shows chain/platform continuing above (or below) what west still
 * holds — copy those rows into west. Does not require a dy detect; covers the
 * common case where elevator self-motion is screen-stable enough that est/deck
 * stay quiet but new VRAM rows appear at the top of the object.
 */
static void FillWestVerticalGapsFromLive(WsShadowLayer *layer, uint32_t tx0,
                                         const uint16_t *live,
                                         const uint8_t *live_occ, int cols,
                                         int rows) {
  if (!layer->entries || cols <= 0 || rows <= 0)
    return;
  const int row_n = rows < kWsLiveMaxRows ? rows : kWsLiveMaxRows;
  const int west_keep = WestKeep(layer);

  for (int64_t tx = (int64_t)tx0 - west_keep; tx < (int64_t)tx0; tx++) {
    if (tx < 0 || tx >= kWsShadowXTiles)
      continue;
    int dist = (int)((int64_t)tx0 - tx);
    int lc = dist - 1;
    if (lc < 0)
      lc = 0;
    if (lc >= cols)
      lc = cols - 1;

    int west_top = -1, west_bot = -1;
    uint16_t tip_tile = 0, bot_tile = 0;
    for (int r = 0; r < row_n; r++) {
      uint16_t e = 0;
      if (!GetEntry(layer, (uint32_t)tx, (uint32_t)r, &e))
        continue;
      if (!IsLiveOpaque(e))
        continue;
      if (west_top < 0) {
        west_top = r;
        tip_tile = e;
      }
      west_bot = r;
      bot_tile = e;
    }
    if (west_top < 0)
      continue;

    /* Live column whose occupancy best matches this west strip on the rows
     * west still holds (same chain/platform column). */
    int src_c = lc;
    int best_ov = -1;
    for (int c = 0; c < cols && c < 12; c++) {
      int ov = 0;
      for (int r = west_top; r <= west_bot; r++) {
        uint16_t e = 0;
        const bool w =
            GetEntry(layer, (uint32_t)tx, (uint32_t)r, &e) && IsLiveOpaque(e);
        if (!w)
          continue;
        if (live_occ[r * kWsLiveMaxCols + c])
          ov += 2;
        else
          ov -= 1;
      }
      if (ov > best_ov) {
        best_ov = ov;
        src_c = c;
      }
    }
    if (best_ov < 2)
      src_c = lc;

    int live_top = -1, live_bot = -1;
    for (int r = 0; r < row_n; r++) {
      if (!live_occ[r * kWsLiveMaxCols + src_c])
        continue;
      if (live_top < 0)
        live_top = r;
      live_bot = r;
    }

    const bool tip_live = live_occ[west_top * kWsLiveMaxCols + src_c] != 0;
    const bool tip_live_b = live_occ[west_bot * kWsLiveMaxCols + src_c] != 0;
    const bool live_higher =
        best_ov >= 2 && live_top >= 0 && live_top < west_top;
    const bool live_lower = best_ov >= 2 && live_bot > west_bot;

    if (tip_live || live_higher) {
      const int from = live_higher ? live_top : 0;
      for (int r = from; r < west_top; r++) {
        uint16_t cur = 0;
        if (GetEntry(layer, (uint32_t)tx, (uint32_t)r, &cur))
          continue;
        const int li = r * kWsLiveMaxCols + src_c;
        if (live_occ[li] && IsLiveOpaque(live[li]))
          SetEntry(layer, (uint32_t)tx, (uint32_t)r, live[li]);
        else if (live_higher && IsLiveOpaque(tip_tile))
          SetEntry(layer, (uint32_t)tx, (uint32_t)r, tip_tile);
      }
    }
    /* Downward grow: when rejectEastEcho (viewport-relative gutters), do not
     * stretch west props below their ROM/history footprint — that shifted
     * chains down vs the 4:3 strip. Upward fill for elevator tops stays. */
    if (!layer->rejectEastEcho && (tip_live_b || live_lower)) {
      const int to = live_lower ? live_bot + 1 : row_n;
      for (int r = west_bot + 1; r < to; r++) {
        uint16_t cur = 0;
        if (GetEntry(layer, (uint32_t)tx, (uint32_t)r, &cur))
          continue;
        const int li = r * kWsLiveMaxCols + src_c;
        if (live_occ[li] && IsLiveOpaque(live[li]))
          SetEntry(layer, (uint32_t)tx, (uint32_t)r, live[li]);
        else if (live_lower && IsLiveOpaque(bot_tile))
          SetEntry(layer, (uint32_t)tx, (uint32_t)r, bot_tile);
      }
    }
  }
}

/* Occupancy centroid / edge reveal → viewport dy for elevator self-motion. */
static int DetectTopRevealDy(const uint8_t *prev_occ, int prev_cols,
                             int prev_rows, const uint8_t *curr_occ, int cols,
                             int rows) {
  if (prev_cols <= 0 || cols <= 0 || rows < 3 || prev_rows < 3)
    return 0;
  int prev_top = 0, curr_top = 0;
  int prev_bot = 0, curr_bot = 0;
  int prev_n = 0, curr_n = 0;
  int prev_mom = 0, curr_mom = 0;
  const int band = 2;
  for (int r = 0; r < prev_rows && r < kWsLiveMaxRows; r++) {
    int row_n = 0;
    for (int c = 0; c < prev_cols && c < kWsLiveMaxCols; c++)
      if (prev_occ[r * kWsLiveMaxCols + c])
        row_n++;
    prev_n += row_n;
    prev_mom += r * row_n;
    if (r < band)
      prev_top += row_n;
    if (r >= prev_rows - band)
      prev_bot += row_n;
  }
  for (int r = 0; r < rows && r < kWsLiveMaxRows; r++) {
    int row_n = 0;
    for (int c = 0; c < cols && c < kWsLiveMaxCols; c++)
      if (curr_occ[r * kWsLiveMaxCols + c])
        row_n++;
    curr_n += row_n;
    curr_mom += r * row_n;
    if (r < band)
      curr_top += row_n;
    if (r >= rows - band)
      curr_bot += row_n;
  }
  if (prev_n < 8 || curr_n < 8)
    return 0;
  /* Appearing at top → moved down. Appearing at bottom → moved up. */
  if (curr_top >= prev_top + 2 && curr_top >= 3)
    return -1;
  if (curr_bot >= prev_bot + 2 && curr_bot >= 3)
    return 1;
  /* Centroid shift of at least ~half a tile row. */
  const int prev_cy = (prev_mom * 2 + prev_n) / (prev_n * 2);
  const int curr_cy = (curr_mom * 2 + curr_n) / (curr_n * 2);
  if (curr_cy >= prev_cy + 1)
    return -1;
  if (curr_cy + 1 <= prev_cy)
    return 1;
  return 0;
}

static int ScoreLiveViewportDy(const uint16_t *prev, const uint8_t *prev_occ,
                               int prev_cols, int prev_rows, uint32_t prev_tx0,
                               const uint16_t *curr, const uint8_t *curr_occ,
                               int cols, int rows, uint32_t tx0, int dy,
                               int *out_denom) {
  if (out_denom)
    *out_denom = 0;
  if (prev_cols <= 0 || prev_rows <= 0 || cols <= 0 || rows <= 0)
    return -1;
  int64_t x0 = (int64_t)prev_tx0;
  int64_t x1 = x0 + prev_cols;
  if ((int64_t)tx0 > x0)
    x0 = (int64_t)tx0;
  if ((int64_t)tx0 + cols < x1)
    x1 = (int64_t)tx0 + cols;
  if (x1 - x0 < 4)
    return -1;
  int score = 0;
  int denom = 0;
  for (int64_t tx = x0; tx < x1; tx++) {
    const int pc = (int)(tx - (int64_t)prev_tx0);
    const int cc = (int)(tx - (int64_t)tx0);
    for (int r = 0; r < rows; r++) {
      const int pr = r + dy;
      if (pr < 0 || pr >= prev_rows)
        continue;
      const int pi = pr * kWsLiveMaxCols + pc;
      const int ci = r * kWsLiveMaxCols + cc;
      if (!prev_occ[pi] && !curr_occ[ci])
        continue;
      denom++;
      if (prev_occ[pi] && curr_occ[ci] && prev[pi] == curr[ci])
        score += 2;
      else if (prev_occ[pi] == curr_occ[ci])
        score += 1;
    }
  }
  if (out_denom)
    *out_denom = denom;
  if (denom < 16)
    return -1;
  return score;
}

static int EstimateLiveViewportDy(const uint16_t *prev, const uint8_t *prev_occ,
                                  int prev_cols, int prev_rows,
                                  uint32_t prev_tx0, const uint16_t *curr,
                                  const uint8_t *curr_occ, int cols, int rows,
                                  uint32_t tx0, int *out_best, int *out_id) {
  if (out_best)
    *out_best = -1;
  if (out_id)
    *out_id = -1;
  int best_dy = 0;
  int best_score = -1;
  int identity = -1;
  for (int dy = -kWsLiveDyMax; dy <= kWsLiveDyMax; dy++) {
    int denom = 0;
    const int score =
        ScoreLiveViewportDy(prev, prev_occ, prev_cols, prev_rows, prev_tx0,
                            curr, curr_occ, cols, rows, tx0, dy, &denom);
    if (score < 0)
      continue;
    if (score > best_score) {
      best_score = score;
      best_dy = dy;
    }
    if (dy == 0)
      identity = score;
  }
  if (out_best)
    *out_best = best_score;
  if (out_id)
    *out_id = identity;
  if (best_dy == 0 || best_score < 0)
    return 0;
  if (identity >= 0 && best_score <= identity)
    return 0;
  if (identity >= 0 && best_score < identity + 2)
    return 0;
  return best_dy;
}

/* Widest near-solid row — platforms/gates span many cols; chains do not.
 * Bottom-most n>=3 was pinned on persistent floor/edge rows while the
 * elevator deck moved above them, so west never got a shift. */
static int OccDeckRow(const uint8_t *occ, int cols, int rows) {
  int best_r = -1;
  int best_n = 0;
  for (int r = 0; r < rows; r++) {
    int n = 0;
    for (int c = 0; c < cols; c++) {
      if (occ[r * kWsLiveMaxCols + c])
        n++;
    }
    if (n >= 5 && n >= best_n) {
      best_n = n;
      best_r = r;
    }
  }
  return best_r;
}

/* Live-strip anchor: widest band, else bottom-most opaque. */
static int LiveAnchorRow(const uint8_t *occ, int cols, int rows) {
  int wide = OccDeckRow(occ, cols, rows);
  if (wide >= 0)
    return wide;
  int bottom = -1;
  for (int r = 0; r < rows; r++) {
    for (int c = 0; c < cols; c++) {
      if (occ[r * kWsLiveMaxCols + c])
        bottom = r;
    }
  }
  return bottom;
}

/* Occupancy mass dy — survives elevator tile-ID animation. */
static int EstimateOccMassDy(const uint8_t *prev_occ, int prev_cols,
                             int prev_rows, const uint8_t *curr_occ, int cols,
                             int rows) {
  if (prev_cols <= 0 || prev_rows <= 0 || cols <= 0 || rows <= 0)
    return 0;
  int prev_row[kWsLiveMaxRows];
  int curr_row[kWsLiveMaxRows];
  memset(prev_row, 0, sizeof(prev_row));
  memset(curr_row, 0, sizeof(curr_row));
  int prev_n = 0, curr_n = 0;
  for (int r = 0; r < prev_rows && r < kWsLiveMaxRows; r++) {
    for (int c = 0; c < prev_cols; c++) {
      if (prev_occ[r * kWsLiveMaxCols + c]) {
        prev_row[r]++;
        prev_n++;
      }
    }
  }
  for (int r = 0; r < rows && r < kWsLiveMaxRows; r++) {
    for (int c = 0; c < cols; c++) {
      if (curr_occ[r * kWsLiveMaxCols + c]) {
        curr_row[r]++;
        curr_n++;
      }
    }
  }
  if (prev_n < 8 || curr_n < 8)
    return 0;
  int best_dy = 0;
  int best = -1;
  int identity = -1;
  for (int dy = -kWsLiveDyMax; dy <= kWsLiveDyMax; dy++) {
    int score = 0;
    for (int r = 0; r < rows && r < kWsLiveMaxRows; r++) {
      const int pr = r + dy;
      if (pr < 0 || pr >= prev_rows)
        continue;
      const int a = prev_row[pr];
      const int b = curr_row[r];
      score += a < b ? a : b;
    }
    if (score > best) {
      best = score;
      best_dy = dy;
    }
    if (dy == 0)
      identity = score;
  }
  if (best_dy == 0 || best < 0)
    return 0;
  if (identity >= 0 && best < identity + 3)
    return 0;
  return best_dy > 1 ? 1 : best_dy < -1 ? -1 : best_dy;
}

void WsShadowFrame(const struct Ppu *ppu) {
  for (int i = 0; i < kLayers; i++) {
    WsShadowLayer *layer = &s_layers[i];
    layer->active = layer->registered;
    layer->registered = false;
    if (!layer->active)
      continue;

    layer->wide = PPU_bgTilemapWider(ppu, i) != 0;
    layer->tileShift = PPU_bigTiles(ppu, i) ? 4 : 3;
    layer->worldSet = false;
    if (layer->fold) {
      layer->foldVram = ppu->vram;
      memset(layer->foldRow, 0, sizeof(layer->foldRow));
    }
    if (!layer->entries)
      continue;

    const uint16_t map_base_now = (uint16_t)PPU_bgTilemapAdr(ppu, i);
    if (layer->retainHistory) {
      const bool keep = layer->haveRetainMapBase &&
                        layer->retainMapBase == map_base_now;
      if (!keep) {
        if (layer->valid)
          memset(layer->valid, 0, kWsShadowXTiles * kWsShadowYTiles / 8);
        if (layer->guessOrigin)
          memset(layer->guessOrigin, 0, kWsShadowXTiles * kWsShadowYTiles / 8);
        if (layer->cooldown)
          memset(layer->cooldown, 0,
                 (size_t)kWsShadowXTiles * kWsShadowYTiles);
        layer->validCount = 0;
        layer->haveLastOrigin = false;
        layer->havePrevLive = false;
      }
      layer->retainMapBase = map_base_now;
      layer->haveRetainMapBase = true;
    } else {
      /* Ordinary world-keyed layers deliberately retain exact tiles across
       * camera movement; that is the current engine's history-first margin
       * contract. The specialized viewport-relative mode above resets on a
       * tilemap-base scene change. */
      layer->haveRetainMapBase = false;
    }
    layer->mapBaseWord = map_base_now;

    const unsigned sh = layer->tileShift;
    const int view_cols = 256 >> sh;
    const unsigned phase = (unsigned)layer->worldX & ((1u << sh) - 1u);
    int cols = view_cols + (phase ? 1 : 0);
    if (layer->captureCols > cols)
      cols = layer->captureCols;
    const int max_cols = layer->wide ? 64 : 32;
    if (cols > max_cols)
      cols = max_cols;
    if (cols > kWsLiveMaxCols)
      cols = kWsLiveMaxCols;
    const int rows = sh == 4 ? 16 : 29;
    const uint32_t tx0 = layer->worldX >> sh;
    const uint32_t ty0 = layer->worldY >> sh;
    const uint32_t buf_tx0 = layer->scrollX >> sh;
    const uint32_t buf_ty0 = layer->scrollY >> sh;
    const int x_mask = layer->wide ? 63 : 31;

    /*
     * retainHistory BG2: viewport-row Y keys.
     *
     * Elevator/gate motion is a VRAM window rewrite — viewport keys keep the
     * left gutter in sync when we shift west by the detected live dy.
     *
     * Look up/down also rewrites the live window (PPU vs is fine-only). When
     * live occupancy scrolls with worldY, shift west by that dty so the
     * gutter does not ride the camera. Screen-stable follow (identity wins)
     * leaves west alone — viewport keys are already correct.
     */
    uint16_t live_e[kWsLiveMaxCols * kWsLiveMaxRows];
    uint8_t live_occ[kWsLiveMaxCols * kWsLiveMaxRows];
    if (layer->retainHistory)
      memset(live_occ, 0, sizeof(live_occ));

    for (int col = 0; col < cols; col++) {
      int mapCol = (int)((buf_tx0 + (uint32_t)col) & (uint32_t)x_mask);
      int half = layer->wide && mapCol >= 32;
      for (int row = 0; row < rows; row++) {
        int mapRow = (int)((buf_ty0 + (uint32_t)row) & 31u);
        uint16_t word = (uint16_t)(layer->mapBaseWord + (half ? 0x400 : 0) +
                                   (mapRow << 5) + (mapCol & 31));
        const uint16_t tile = ppu->vram[word & 0x7fff];
        if (layer->retainHistory) {
          if (col < kWsLiveMaxCols && row < kWsLiveMaxRows) {
            const int li = row * kWsLiveMaxCols + col;
            live_e[li] = tile;
            live_occ[li] = IsLiveOpaque(tile) ? 1 : 0;
          }
        } else {
          SetEntry(layer, tx0 + (uint32_t)col, ty0 + (uint32_t)row, tile);
        }
      }
    }

    if (layer->retainHistory) {
      /*
       * Shift west only when the LIVE strip itself moves in viewport space
       * (elevator/gate VRAM rewrite, or look rewrite). Do not compare west
       * anchors to live — different objects / chain-vs-deck rows false-fire
       * and desync a chain pair split across the 4:3 edge.
       *
       * Big-tile fine wrap (sy 15→0) makes tile correlation prefer dy=±1
       * while the deck stays put; ignore est/deck on those frames.
       * Camera-follow: identity wins / deck stable → apply 0.
       */
      const unsigned fine_mask = (1u << sh) - 1u;
      const unsigned fine = (unsigned)layer->scrollY & fine_mask;
      const unsigned prev_fine = (unsigned)layer->prevScrollY & fine_mask;
      const bool fine_wrap =
          layer->havePrevLive &&
          ((prev_fine >= fine_mask - 2u && fine <= 2u) ||
           (fine >= fine_mask - 2u && prev_fine <= 2u));
      int apply = 0;
      int dy_est = 0, dy_deck = 0, dy_mass = 0, dy_reveal = 0, dy_world = 0;
      int best_sc = -1, id_sc = -1;
      const int live_deck = LiveAnchorRow(live_occ, cols, rows);

      if (layer->haveLastOrigin) {
        const int32_t dty = (int32_t)ty0 - (int32_t)layer->lastTy0;
        if (dty >= -kWsLiveDyMax && dty <= kWsLiveDyMax && dty != 0)
          dy_world = (int)dty;
      }

      if (layer->havePrevLive && !fine_wrap) {
        dy_est = EstimateLiveViewportDy(
            layer->prevLive, layer->prevLiveOcc, layer->prevLiveCols,
            layer->prevLiveRows, layer->prevLiveTx0, live_e, live_occ, cols,
            rows, tx0, &best_sc, &id_sc);
        dy_mass = EstimateOccMassDy(layer->prevLiveOcc, layer->prevLiveCols,
                                    layer->prevLiveRows, live_occ, cols, rows);
        dy_reveal = DetectTopRevealDy(layer->prevLiveOcc, layer->prevLiveCols,
                                      layer->prevLiveRows, live_occ, cols,
                                      rows);
        const int prev_d = LiveAnchorRow(
            layer->prevLiveOcc, layer->prevLiveCols, layer->prevLiveRows);
        if (prev_d >= 0 && live_deck >= 0 && prev_d != live_deck) {
          dy_deck = prev_d - live_deck;
          if (dy_deck > 1)
            dy_deck = 1;
          if (dy_deck < -1)
            dy_deck = -1;
        }
      }

      /* Edge flicker can trip reveal while the strip is identity-stable. */
      if (dy_reveal != 0 && id_sc >= 0 && best_sc >= 0 &&
          best_sc <= id_sc + 2)
        dy_reveal = 0;

      /* Elevator/gate VRAM motion first — must win over cam world_dty, which
       * often steps the opposite way during ride/follow and caused snaps.
       * rejectEastEcho (MW viewport-relative idle BG2): only honor deck/mass
       * elevator motion. est/reveal/world jitter west ROM-prefilled chains
       * against fine-scroll bob and pan. */
      if (!fine_wrap && dy_deck != 0) {
        apply = dy_deck;
      } else if (!fine_wrap && dy_mass != 0) {
        apply = dy_mass;
      } else if (!layer->rejectEastEcho && !fine_wrap && dy_reveal != 0) {
        apply = dy_reveal;
      } else if (!layer->rejectEastEcho && !fine_wrap && dy_est != 0) {
        apply = dy_est > 1 ? 1 : dy_est < -1 ? -1 : dy_est;
      } else if (!layer->rejectEastEcho && dy_world != 0 &&
                 layer->havePrevLive) {
        /* Look: seam scrolled with cam. Identity win/tie = follow. */
        const int seam_cols = cols < 4 ? cols : 4;
        int den_w = 0, den_id = 0;
        const int s_world = ScoreLiveViewportDy(
            layer->prevLive, layer->prevLiveOcc, layer->prevLiveCols,
            layer->prevLiveRows, layer->prevLiveTx0, live_e, live_occ,
            seam_cols, rows, tx0, dy_world, &den_w);
        const int s_id = ScoreLiveViewportDy(
            layer->prevLive, layer->prevLiveOcc, layer->prevLiveCols,
            layer->prevLiveRows, layer->prevLiveTx0, live_e, live_occ,
            seam_cols, rows, tx0, 0, &den_id);
        if (s_id >= 0 && (s_world < 0 || s_id >= s_world))
          apply = 0;
        else
          apply = dy_world > 1 ? 1 : dy_world < -1 ? -1 : dy_world;
        (void)den_w;
        (void)den_id;
      }

      {
        static int ylog;
        const char *e = getenv("SNESRECOMP_WS_YLOG");
        if (e && e[0] == '1' && ylog < 200 &&
            (apply != 0 || dy_est != 0 || dy_deck != 0 || dy_mass != 0 ||
             dy_reveal != 0 || dy_world != 0 || ylog < 16)) {
          ylog++;
          fprintf(stderr,
                  "[ws_ylog] est=%d mass=%d reveal=%d deck_d=%d world_dty=%d "
                  "apply=%d ldeck=%d best=%d id=%d wy=%u sy=%u wrap=%d\n",
                  dy_est, dy_mass, dy_reveal, dy_deck, dy_world, apply,
                  live_deck, best_sc, id_sc, layer->worldY, layer->scrollY,
                  fine_wrap ? 1 : 0);
        }
      }
      if (apply != 0) {
        ShiftWestViewportRows(layer, tx0, apply, rows);
        BackfillWestVacatedRows(layer, tx0, live_e, live_occ, cols, rows,
                                apply);
      }
      /* Grow west toward live vertical extent (missing chain tops, etc.). */
      FillWestVerticalGapsFromLive(layer, tx0, live_e, live_occ, cols, rows);
      layer->lastTy0 = ty0;
      layer->lastTx0 = tx0;
      layer->haveLastOrigin = true;

      const int west_keep = WestKeep(layer);
      for (int col = 0; col < cols; col++) {
        const uint32_t tx = tx0 + (uint32_t)col;
        /* East-of-view capture that merely echoes a live view column is a
         * wrap/period phantom (second door in the right gutter). Drop it. */
        bool east_echo = false;
        if (layer->rejectEastEcho && col >= view_cols) {
          for (int vc = 0; vc < view_cols && !east_echo; vc++) {
            int same = 1;
            for (int row = 0; row < rows; row++) {
              if (col >= kWsLiveMaxCols || row >= kWsLiveMaxRows ||
                  vc >= kWsLiveMaxCols) {
                same = 0;
                break;
              }
              const int a = row * kWsLiveMaxCols + col;
              const int b = row * kWsLiveMaxCols + vc;
              if (live_e[a] != live_e[b] || live_occ[a] != live_occ[b]) {
                same = 0;
                break;
              }
            }
            if (same)
              east_echo = true;
          }
        }
        for (int row = 0; row < rows; row++) {
          if (col >= kWsLiveMaxCols || row >= kWsLiveMaxRows)
            break;
          if (east_echo)
            ClearEntry(layer, tx, (uint32_t)row);
          else
            SetEntry(layer, tx, (uint32_t)row,
                     live_e[row * kWsLiveMaxCols + col]);
        }
      }

      layer->prevLiveCols = cols < kWsLiveMaxCols ? cols : kWsLiveMaxCols;
      layer->prevLiveRows = rows < kWsLiveMaxRows ? rows : kWsLiveMaxRows;
      layer->prevLiveTx0 = tx0;
      layer->prevScrollY = layer->scrollY;
      for (int col = 0; col < layer->prevLiveCols; col++) {
        for (int row = 0; row < layer->prevLiveRows; row++) {
          const int li = row * kWsLiveMaxCols + col;
          layer->prevLive[li] = live_e[li];
          layer->prevLiveOcc[li] = live_occ[li];
        }
      }
      layer->havePrevLive = true;

      const int east_keep = EastKeep(layer);
      const int64_t wx0 = (int64_t)tx0, wx1 = (int64_t)tx0 + cols;
      const int64_t prune_lo = wx0 - west_keep - 16;
      /* east_keep == 0 must reproduce the original bound (wx1 + 8) EXACTLY,
       * or every existing game starts clearing more east history than before. */
      const int64_t prune_hi = wx1 + east_keep + 8;
      const int64_t row_hi = (int64_t)rows + 4;
      for (int64_t tx = prune_lo; tx < prune_hi; tx++) {
        if (tx < 0 || tx >= kWsShadowXTiles)
          continue;
        if (tx >= wx0 - west_keep && tx < wx1 + east_keep)
          continue;
        for (int64_t ty = 0; ty < row_hi && ty < kWsShadowYTiles; ty++)
          ClearEntry(layer, (uint32_t)tx, (uint32_t)ty);
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
  /*
   * The 8x8 background renderers call once per tile chunk. A hardware window
   * can make the final chunk start inside the native viewport but extend into
   * the right margin (for example x=255..262). Treat that whole straddling
   * chunk as world-keyed shadow data; returning the circular VRAM tile inserts
   * an exact one-tile seam before margin rendering begins.
   */
  const int tile_pixels = 1 << (layer->tileShift ? layer->tileShift : 3);
  if (!layer->active ||
      (screenX >= 0 && screenX + tile_pixels <= 256))
    return realTile;

  /* Layered margin sources, exact-first: (1) the world-keyed history —
   * captured from the native view sweep AND from the game's own
   * uploads at write time, it holds the exact world content whenever
   * that content has ever existed in VRAM; (2) the periodic fold for
   * cells never seen (stage start, beyond the populated span) on rows
   * whose native window proves an exact period; (3) the renderer's
   * plain map wrap. History-first keeps world-anchored features
   * (towers) in the margins and stops fold/history flicker: a false
   * period inferred from a feature-free native window can no longer
   * paint filler over known feature cells. */
  const uint32_t shift = layer->tileShift ? layer->tileShift : 3;
  int32_t worldX = (int32_t)layer->worldX + screenX;
  int32_t worldY =
      layer->retainHistory
          ? (int32_t)(((wrappedY - layer->scrollY) & 0x3ff) +
                      (layer->scrollY & ((1u << shift) - 1)))
          : (int32_t)(layer->worldY +
                      ((wrappedY - layer->scrollY) & 0x3ff));
  if (layer->entries && worldX >= 0 && worldY >= 0) {
    uint16_t entry;
    const bool hit = GetEntry(layer, (uint32_t)worldX >> shift,
                              (uint32_t)worldY >> shift, &entry);
    /* Always-on margin hit/miss accounting, split by side. Cheap counters, no
     * arming: without them "the gutter looks the same" is indistinguishable
     * from "history was never consulted" from "history was consulted and
     * missed". */
    WsShadowMarginStat *st = &s_marginStats[layerIndex];
    if (screenX < 0) {
      if (hit) st->westHit++; else st->westMiss++;
    } else {
      if (hit) st->eastHit++; else st->eastMiss++;
    }
    if (hit)
      return entry;
  }

  if (layer->fold && layer->foldVram) {
    uint16_t off = (uint16_t)(mapWordAdr - layer->mapBaseWord);
    if (off < 0x800) {
      int col = (off & 0x1f) | (off & 0x400 ? 0x20 : 0);
      int row = (off >> 5) & 0x1f;
      int natCol = (hScroll >> shift) & 63;
      uint8_t period = FoldRowPeriod(layer, row, natCol);
      if (period) {
        int rel = (col - natCol) & 63;
        if (rel >= 32) {
          if (screenX < 0)
            s_marginStats[layerIndex].westFold++;
          else
            s_marginStats[layerIndex].eastFold++;
          return FoldMapEntry(layer, row, (natCol + rel % period) & 63);
        }
        return realTile;  /* native column (or margin overlapping it) */
      }
    }
  }

  if (layer->blankTilePlus1) {
    if (screenX < 0)
      s_marginStats[layerIndex].westBlank++;
    else
      s_marginStats[layerIndex].eastBlank++;
    return (uint16_t)(layer->blankTilePlus1 - 1);
  }
  if (screenX < 0)
    s_marginStats[layerIndex].westRawFallback++;
  else
    s_marginStats[layerIndex].eastRawFallback++;
  return realTile;
}

bool WsShadowLayerActive(int layerIndex) {
  return layerIndex >= 0 && layerIndex < kLayers &&
         s_layers[layerIndex].active &&
         (s_layers[layerIndex].fold || s_layers[layerIndex].entries);
}

uint32_t WsShadowWorldX(int layerIndex) {
  if (layerIndex < 0 || layerIndex >= kLayers)
    return 0;
  return s_layers[layerIndex].worldX;
}

uint32_t WsShadowWorldY(int layerIndex) {
  if (layerIndex < 0 || layerIndex >= kLayers)
    return 0;
  return s_layers[layerIndex].worldY;
}

uint32_t WsShadowPresentWorldY(int layerIndex, int screenX) {
  (void)screenX;
  if (layerIndex < 0 || layerIndex >= kLayers)
    return 0;
  if (s_layers[layerIndex].retainHistory)
    return s_layers[layerIndex].scrollY;
  return s_layers[layerIndex].worldY;
}

uint32_t WsShadowScrollX(int layerIndex) {
  if (layerIndex < 0 || layerIndex >= kLayers)
    return 0;
  return s_layers[layerIndex].scrollX;
}

uint32_t WsShadowScrollY(int layerIndex) {
  if (layerIndex < 0 || layerIndex >= kLayers)
    return 0;
  return s_layers[layerIndex].scrollY;
}
