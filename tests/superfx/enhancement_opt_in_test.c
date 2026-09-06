/* Regression coverage for the host-side Super FX enhancement boundary.
 * No game ROM, generated data, or platform frontend is required. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snes/superfx.h"

enum { kRomSize = 65536, kRamSize = 65536 };

static int failures;

static void check(int condition, const char *message) {
  if (!condition) {
    fprintf(stderr, "FAIL: %s\n", message);
    failures++;
  }
}

static SuperFx *make_superfx(uint8_t **rom_out, uint8_t **ram_out) {
  uint8_t *rom = (uint8_t *)calloc(kRomSize, 1);
  uint8_t *ram = (uint8_t *)calloc(kRamSize, 1);
  if (!rom || !ram) {
    free(rom);
    free(ram);
    return NULL;
  }

  /* PLOT x=4,y=3; decrement x; ALT1; RPIX; STOP. This exercises the native
   * pixel cache in both instances without invoking an enhanced replay. */
  static const uint8_t program[] = {0x4c, 0xe1, 0x3d, 0x4c, 0x00};
  memcpy(rom, program, sizeof(program));

  SuperFx *fx = superfx_create(rom, kRomSize, ram, kRamSize);
  if (!fx) {
    free(rom);
    free(ram);
    return NULL;
  }
  fx->colr = 3;
  fx->r[1].data = 4;
  fx->r[2].data = 3;
  *rom_out = rom;
  *ram_out = ram;
  return fx;
}

static void start_at_zero(SuperFx *fx) {
  superfx_cpu_write_io(fx, 0x301e, 0);
  superfx_cpu_write_io(fx, 0x301f, 0);
  superfx_sync(fx, 10000);
}

static void destroy_fixture(SuperFx *fx, uint8_t *rom, uint8_t *ram) {
  superfx_destroy(fx);
  free(rom);
  free(ram);
}

static unsigned prepared, completed;
static bool prepare_replay(void *context, const SuperFx *source, uint8_t *ram) {
  (void)context;
  check(ram != source->ram, "replay callback gets private RAM");
  check(source->ram[0x100] == 3, "native color input remains original");
  ram[0x100] = 1;
  prepared++;
  return true;
}
static void complete_replay(void *context, const SuperFx *result) {
  (void)context;
  check(result != NULL, "presentation replay terminates");
  if (result) {
    check(result->r[0].data == 1, "replay plots and reads its changed color");
    check(result->presentation == NULL, "replay cannot recursively capture itself");
  }
  completed++;
}

static void test_private_replay(void) {
  uint8_t *rom[2], *ram[2];
  SuperFx *fx[2];
  const uint8_t program[] = {0x43, 0x4e, 0x4c, 0xe1, 0x3d, 0x4c, 0x00};
  for (unsigned i = 0; i < 2; i++) {
    fx[i] = make_superfx(&rom[i], &ram[i]);
    if (!fx[i]) abort();
    memcpy(rom[i], program, sizeof(program));
    fx[i]->r[3].data = 0x100;
    ram[i][0x100] = 3;
  }
  check(!superfx_set_presentation_replay(fx[0], 0, 0, prepare_replay,
                                        complete_replay, NULL) &&
        fx[0]->presentation == NULL,
        "faithful mode rejects replay configuration without allocating");
  SuperFx extra;
  check(!superfx_replay_snapshot(fx[0], ram[1], &extra),
        "faithful snapshots reject additional replay passes");
  superfx_set_enhancement_mode(fx[1], kSuperFxEnhancement_PresentationReplay);
  check(superfx_set_presentation_replay(fx[1], 0, 0, prepare_replay,
                                       complete_replay, NULL), "configure replay");
  check(!superfx_replay_snapshot(fx[1], ram[1], &extra),
        "additional replay rejects the authoritative RAM buffer");
  check(!superfx_replay_snapshot(fx[1], ram[0], fx[1]),
        "additional replay rejects overwriting its source core");
  start_at_zero(fx[0]);
  start_at_zero(fx[1]);
  check(prepared == 1 && completed == 1, "matching task completes exactly once");
  check(fx[0]->r[0].data == 3, "original task reads original plotted color");
  check(memcmp(ram[0], ram[1], kRamSize) == 0, "replay preserves every native RAM byte");
  check(memcmp((uint8_t *)fx[0] + offsetof(SuperFx, r),
               (uint8_t *)fx[1] + offsetof(SuperFx, r),
               offsetof(SuperFx, enhancement_mode) - offsetof(SuperFx, r)) == 0,
        "private replay preserves every architectural register, cache and clock");
  superfx_reset(fx[1]);
  superfx_set_enhancement_mode(fx[1], kSuperFxEnhancement_None);
  start_at_zero(fx[1]);
  check(prepared == 1 && completed == 1, "disabled replay never calls title callbacks");
  for (unsigned i = 0; i < 2; i++) destroy_fixture(fx[i], rom[i], ram[i]);
}

int main(void) {
  test_private_replay();
  uint8_t *native_rom = NULL, *native_ram = NULL;
  uint8_t *optin_rom = NULL, *optin_ram = NULL;
  SuperFx *native = make_superfx(&native_rom, &native_ram);
  SuperFx *optin = make_superfx(&optin_rom, &optin_ram);
  if (!native || !optin) {
    fputs("FAIL: could not allocate Super FX fixtures\n", stderr);
    destroy_fixture(native, native_rom, native_ram);
    destroy_fixture(optin, optin_rom, optin_ram);
    return 1;
  }

  check(superfx_get_enhancement_mode(native) == kSuperFxEnhancement_None,
        "new cores default to faithful Super FX");
  superfx_set_widescreen(native, 32, 0x00, 0x0000, 0x100, 0x102, 64);
  check(native->ws_extra == 0 && native->ws_pixels == NULL,
        "widescreen configuration is inert without explicit enhancement");

  superfx_set_enhancement_mode(
      optin, kSuperFxEnhancement_WidescreenLinearProjection);
  superfx_set_widescreen(optin, 32, 0x00, 0x0000, 0x100, 0x102, 64);
  check(optin->ws_extra == 32 && optin->ws_pixels != NULL &&
            optin->ws_task_state != NULL,
        "explicit enhancement allocates presentation-only replay state");

  /* Match the configured task and give it a tiny valid native viewport. The
   * enhanced clone will execute, but it must not touch authoritative state. */
  native_ram[0x100] = optin_ram[0x100] = 4;
  native_ram[0x102] = optin_ram[0x102] = 7;
  start_at_zero(native);
  start_at_zero(optin);
  check(optin->ws_pending_ready,
        "matching task produces an enhanced presentation replay");
  check(memcmp(native_ram, optin_ram, kRamSize) == 0,
        "enhanced replay does not alter native GSU RAM");
  check(memcmp((const uint8_t *)native + offsetof(SuperFx, r),
               (const uint8_t *)optin + offsetof(SuperFx, r),
               offsetof(SuperFx, enhancement_mode) - offsetof(SuperFx, r)) ==
            0,
        "enhanced replay does not alter architectural GSU execution");

  optin->ws_frame_ready = true;
  optin->ws_pending_ready = true;
  optin->ws_render_active = true;
  superfx_set_enhancement_mode(optin, kSuperFxEnhancement_None);
  check(optin->ws_extra == 0 && !optin->ws_frame_ready &&
            !optin->ws_pending_ready && !optin->ws_render_active,
        "returning to faithful mode discards all enhanced presentation state");
  check(!superfx_get_widescreen_frame(optin, NULL, NULL, NULL, NULL),
        "faithful mode never exposes an enhanced frame");

  destroy_fixture(native, native_rom, native_ram);
  destroy_fixture(optin, optin_rom, optin_ram);
  if (failures)
    return 1;
  puts("enhancement_opt_in_test: PASS");
  return 0;
}
