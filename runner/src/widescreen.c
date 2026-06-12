#include "widescreen.h"

#include <string.h>

// NOTE: g_ws_active / g_ws_extra are *declared* in widescreen.h but defined by
// each game (next to its config wiring), not here. Keeping the storage per-game
// means a title that already defines them locally (SMW) needs no change when it
// adopts this asset, while still sharing the contract + helper below. The
// genuinely game-agnostic, identical-across-titles pieces are kWsExtraMax and
// RtlWidescreenPresent.

void RtlWidescreenPresent(uint8_t *dst, size_t pitch, const uint8_t *src,
                          int snes_width, int snes_height) {
  size_t row_bytes = (size_t)snes_width * 4;
  for (int y = 0; y < snes_height; y++)
    memcpy(dst + (size_t)y * pitch, src + (size_t)y * row_bytes, row_bytes);
}
