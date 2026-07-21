#include "widescreen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// NOTE: g_ws_active / g_ws_extra are *declared* in widescreen.h but defined by
// each game (next to its config wiring), not here. Keeping the storage per-game
// means a title that already defines them locally (SMW) needs no change when it
// adopts this asset, while still sharing the contract + helper below. The
// genuinely game-agnostic, identical-across-titles pieces are kWsExtraMax and
// RtlWidescreenPresent.

// Debug frame capture: when SNESRECOMP_FRAME_BMP=<path> is set, rewrite that
// BMP with the composed frame every 60 presents. Host-only observability —
// available in every build config, no debug server required.
static void WsDebugDumpBmp(const char *path, const uint8_t *src,
                           int w, int h) {
  FILE *f = fopen(path, "wb");
  if (!f)
    return;
  uint32_t img = (uint32_t)w * h * 4, off = 14 + 40, size = off + img;
  uint8_t hdr[54] = { 'B', 'M' };
  memcpy(hdr + 2, &size, 4); memcpy(hdr + 10, &off, 4);
  uint32_t ih = 40; int32_t ww = w, hh = -h; uint16_t planes = 1, bpp = 32;
  memcpy(hdr + 14, &ih, 4); memcpy(hdr + 18, &ww, 4); memcpy(hdr + 22, &hh, 4);
  memcpy(hdr + 26, &planes, 2); memcpy(hdr + 28, &bpp, 2);
  memcpy(hdr + 34, &img, 4);
  fwrite(hdr, 1, 54, f);
  fwrite(src, 1, img, f);
  fclose(f);
}

void RtlWidescreenPresent(uint8_t *dst, size_t pitch, const uint8_t *src,
                          int snes_width, int snes_height) {
  size_t row_bytes = (size_t)snes_width * 4;
  for (int y = 0; y < snes_height; y++)
    memcpy(dst + (size_t)y * pitch, src + (size_t)y * row_bytes, row_bytes);

  static int dump_checked;
  static const char *dump_path;
  static int dump_frame = -1;
  static int dumped_target;
  static const char *dump_dir;
  static int dump_dir_next = -1;
  static int dump_dir_end = -1;
  static int dump_dir_step = 60;
  static unsigned present_count;
  if (!dump_checked) {
    dump_checked = 1;
    dump_path = getenv("SNESRECOMP_FRAME_BMP");
    const char *frame = getenv("SNESRECOMP_FRAME_BMP_FRAME");
    if (frame && *frame)
      dump_frame = atoi(frame);
    dump_dir = getenv("SNESRECOMP_FRAME_BMP_DIR");
    if (dump_dir && *dump_dir) {
      const char *start = getenv("SNESRECOMP_FRAME_BMP_START");
      const char *end = getenv("SNESRECOMP_FRAME_BMP_END");
      const char *step = getenv("SNESRECOMP_FRAME_BMP_STEP");
      dump_dir_next = start && *start ? atoi(start) : 0;
      dump_dir_end = end && *end ? atoi(end) : 0x7fffffff;
      dump_dir_step = step && *step ? atoi(step) : 60;
      if (dump_dir_step <= 0)
        dump_dir_step = 60;
    }
  }
  extern int snes_frame_counter;
  if (dump_path) {
    if (dump_frame >= 0 && !dumped_target &&
        snes_frame_counter >= dump_frame) {
      WsDebugDumpBmp(dump_path, src, snes_width, snes_height);
      dumped_target = 1;
    } else if (dump_frame < 0 && (++present_count % 60) == 0) {
      WsDebugDumpBmp(dump_path, src, snes_width, snes_height);
    }
  }
  if (dump_dir && *dump_dir && dump_dir_next >= 0) {
    while (snes_frame_counter >= dump_dir_next &&
           dump_dir_next <= dump_dir_end) {
      char path[512];
      snprintf(path, sizeof(path), "%s/frame_%06d.bmp", dump_dir,
               dump_dir_next);
      WsDebugDumpBmp(path, src, snes_width, snes_height);
      dump_dir_next += dump_dir_step;
    }
  }
}
