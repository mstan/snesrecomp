#include "widescreen.h"

#include <string.h>

// Canonical storage for the widescreen master switch. A game's config sets
// these once at startup (default off => byte-identical to the faithful build).
// Single definition here so the injector-emitted `extern bool g_ws_active;`
// references resolve to the same symbol in every title.
bool g_ws_active = false;
int g_ws_extra = 0;

void RtlWidescreenPresent(uint8_t *dst, size_t pitch, const uint8_t *src,
                          int snes_width, int snes_height) {
  size_t row_bytes = (size_t)snes_width * 4;
  for (int y = 0; y < snes_height; y++)
    memcpy(dst + (size_t)y * pitch, src + (size_t)y * row_bytes, row_bytes);
}
