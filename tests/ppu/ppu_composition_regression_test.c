/* Differential coverage for PpuDrawWholeLine composition behavior. This
 * includes ppu.c directly so candidate renderer changes can be compared
 * against a separately compiled HEAD source without exporting test-only API. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "snes/snes.h"

Snes *g_snes;

uint16_t WsShadowTile(int layer, int screen_x, uint32_t wrapped_y,
                      uint16_t h_scroll, uint16_t map_word_adr,
                      uint16_t real_tile) {
  (void)layer;
  (void)screen_x;
  (void)wrapped_y;
  (void)h_scroll;
  (void)map_word_adr;
  return real_tile;
}

bool WsShadowLayerActive(int layer) {
  (void)layer;
  return false;
}

uint32_t WsShadowWorldX(int layer) {
  (void)layer;
  return 0;
}

uint32_t WsShadowWorldY(int layer) {
  (void)layer;
  return 0;
}

uint32_t WsShadowPresentWorldY(int layer, int screen_x) {
  (void)layer;
  (void)screen_x;
  return 0;
}

uint32_t WsShadowScrollX(int layer) {
  (void)layer;
  return 0;
}

uint32_t WsShadowScrollY(int layer) {
  (void)layer;
  return 0;
}

void WsShadowOnVramWrite(uint16_t word_adr, uint16_t value) {
  (void)word_adr;
  (void)value;
}

#ifndef PPU_COMPOSITION_PPU_INCLUDE
#define PPU_COMPOSITION_PPU_INCLUDE "../../runner/src/snes/ppu.c"
#endif
#include PPU_COMPOSITION_PPU_INCLUDE

static uint32_t rng_next(uint32_t *state) {
  uint32_t x = *state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  *state = x;
  return x;
}

static int fail_at(const char *name, int iter, int index,
                   uint32_t got, uint32_t want) {
  fprintf(stderr, "FAIL %s iter=%d index=%d got=%08x want=%08x\n",
          name, iter, index, got, want);
  return 1;
}

static int test_color_window_edges_strict(Ppu *ppu) {
  uint32_t rng = 0xa5a55a5au;
  for (int iter = 0; iter < 5000; iter++) {
    ppu_reset(ppu);
    PpuSetExtraSpace(ppu, 32);
    PpuSetExtraSideSpace(ppu, (int)(rng_next(&rng) % 33),
                         (int)(rng_next(&rng) % 33), 0);
    ppu->window1left = (uint8_t)(rng_next(&rng) & 0xff);
    ppu->window1right = (uint8_t)(rng_next(&rng) & 0xff);
    ppu->window2left = (uint8_t)(rng_next(&rng) & 0xff);
    ppu->window2right = (uint8_t)(rng_next(&rng) & 0xff);
    ppu->windowsel = rng_next(&rng) & 0x00ffffffu;
    for (uint layer = 0; layer < 6; layer++) {
      PpuWindows win;
      PpuWindows_Calc(&win, ppu, layer, (int)(rng_next(&rng) % 224));
      if (win.nr < 1 || win.nr > 5)
        return fail_at("window_nr", iter, win.nr, win.nr, 1);
      for (uint i = 0; i < win.nr; i++) {
        if (win.edges[i] >= win.edges[i + 1])
          return fail_at("window_edges", iter, (int)i,
                         (uint32_t)(uint16_t)win.edges[i],
                         (uint32_t)(uint16_t)win.edges[i + 1]);
      }
    }
  }
  return 0;
}

static uint64_t hash_bytes(uint64_t h, const void *data, size_t len) {
  const uint8_t *p = (const uint8_t *)data;
  for (size_t i = 0; i < len; i++) {
    h ^= p[i];
    h *= 1099511628211ull;
  }
  return h;
}

static void fill_render_case(Ppu *ppu, uint32_t *rng) {
  for (int i = 0; i < 256; i++)
    ppu->cgram[i] = (uint16_t)rng_next(rng);
  for (size_t i = 0; i < sizeof(ppu->vram) / sizeof(ppu->vram[0]); i++)
    ppu->vram[i] = (uint16_t)rng_next(rng);
  for (int i = 0; i < 128; i++)
    ppu->oam[i * 2] = 0xf000;
  for (int i = 0; i < 32; i++)
    ppu->highOam[i] = 0;
  ppu->inidisp = 0x0f;
  ppu->bgmode = 1;
  ppu->screenEnabled[0] = 0x17;
  ppu->screenEnabled[1] = 0x03;
  ppu->bgXsc[0] = 0x04;
  ppu->bgXsc[1] = 0x08;
  ppu->bgXsc[2] = 0x0c;
  ppu->bgTileAdr = 0x3210;
}

static uint64_t render_case_digest(Ppu *ppu, int case_id) {
  static uint32_t pixels[kPpuBufWidth * 240];
  static uint32_t overlay[kPpuBufWidth * 240];
  uint32_t rng = 0x1000f00du + (uint32_t)case_id * 0x9e3779b9u;
  ppu_reset(ppu);
  memset(pixels, 0x5a, sizeof(pixels));
  memset(overlay, 0, sizeof(overlay));
  PpuBeginDrawing(ppu, (uint8_t *)pixels, kPpuBufWidth * sizeof(uint32_t),
                  kPpuRenderFlags_NewRenderer);
  fill_render_case(ppu, &rng);

  switch (case_id) {
    case 1:
      ppu->cgwsel = 0x02;
      ppu->cgadsub = 0x7f;
      ppu->fixedColor = 0x4210;
      break;
    case 2:
      ppu->windowsel = (uint32_t)kWindow1Enabled << 20;
      ppu->window1left = 31;
      ppu->window1right = 190;
      ppu->cgwsel = 0x40;
      break;
    case 3:
      PpuSetExtraSpaceCentered(ppu, 24);
      break;
    case 4:
      PpuSetExtraSpaceCentered(ppu, 24);
      PpuSetWidescreenHudSplit(ppu, 24, 48, 208);
      PpuSetWidescreenHudAlwaysVisible(ppu, true);
      break;
    case 5:
      PpuSetExtraSpaceCentered(ppu, 24);
      PpuSetWidescreenHudAlwaysVisible(ppu, true);
      PpuSetWidescreenLayerAnchorBand(ppu, 1, 0, 48, 40, 216);
      break;
    case 6:
      PpuSetExtraSpaceCentered(ppu, 24);
      PpuSetWidescreenLayerRepeat(ppu, 1);
      PpuSetWidescreenLayerRepeatBand(ppu, 0, 0, 48);
      break;
    case 7:
      PpuBindOverlaySurface(ppu, kPpuOverlaySource_Bg1, (uint8_t *)overlay,
                            kPpuBufWidth * sizeof(uint32_t));
      PpuSetOverlayCapture(ppu, kPpuOverlaySource_Bg1, 16, 0, 96, 48, 0);
      break;
    default:
      break;
  }

  ppu_runLine(ppu, 0);
  for (int line = 1; line <= 64; line++)
    ppu_runLine(ppu, line);

  uint64_t h = 1469598103934665603ull;
  h = hash_bytes(h, pixels, sizeof(uint32_t) * kPpuBufWidth * 64);
  h = hash_bytes(h, overlay, sizeof(uint32_t) * kPpuBufWidth * 64);
  h = hash_bytes(h, &ppu->rangeOver, sizeof(ppu->rangeOver));
  h = hash_bytes(h, &ppu->timeOver, sizeof(ppu->timeOver));
  return h;
}

static int print_render_digest(Ppu *ppu) {
  uint64_t h = 1469598103934665603ull;
  for (int i = 0; i < 8; i++) {
    uint64_t case_hash = render_case_digest(ppu, i);
    h = hash_bytes(h, &case_hash, sizeof(case_hash));
  }
  printf("ppu_composition_digest=%016llx\n", (unsigned long long)h);
  return 0;
}

static int run_render_bench(Ppu *ppu, int loops) {
  volatile uint64_t sink = 0;
  clock_t start = clock();
  for (int iter = 0; iter < loops; iter++) {
    for (int case_id = 0; case_id < 8; case_id++) {
      uint64_t case_hash = render_case_digest(ppu, case_id);
      sink = (sink * 1099511628211ull) ^ case_hash ^ (uint64_t)iter;
    }
  }
  clock_t end = clock();
  double ms = 1000.0 * (double)(end - start) / (double)CLOCKS_PER_SEC;
  printf("ppu_composition_render_bench loops=%d ms=%.3f sink=%016llx\n",
         loops, ms, (unsigned long long)sink);
  return 0;
}

int main(int argc, char **argv) {
  Ppu *ppu = ppu_init();
  int failures = 0;
  if (!ppu)
    return 2;
  if (argc > 1 && strcmp(argv[1], "--digest") == 0) {
    failures = print_render_digest(ppu);
    ppu_free(ppu);
    return failures ? 1 : 0;
  }
  if (argc > 1 && strcmp(argv[1], "--render-bench") == 0) {
    int loops = argc > 2 ? atoi(argv[2]) : 100;
    if (loops < 1)
      loops = 1;
    failures = run_render_bench(ppu, loops);
    ppu_free(ppu);
    return failures ? 1 : 0;
  }
  failures += test_color_window_edges_strict(ppu);
  if (failures == 0)
    failures += print_render_digest(ppu);
  ppu_free(ppu);
  if (failures)
    return 1;
  printf("ppu_composition_regression_test: PASS\n");
  return 0;
}
