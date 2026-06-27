/*
 * bsnes_cycles_probe -- end-to-end check of the bsnes Axis-2 cycle hook.
 *
 * Loads the patched bsnes_libretro.dll, boots a ROM headless, and reads the
 * exported bsnes_total_guest_cycles() across frames. A correct hook advances
 * the master-clock counter by ~one frame's worth per retro_run:
 *   NTSC: 21477270 Hz / 60.0988 fps ~= 357366 master cyc/frame
 *   (a frame is 262 scanlines * 1364 master clocks = 357368, minus the short
 *    line on non-interlaced NTSC) -- so expect ~357xxx, NOT 0 and NOT wild.
 *
 * Dev-only oracle tooling. Build (PowerShell / mingw):
 *   gcc -O2 -I F:/Projects/_bsnes_src/bsnes/target-libretro \
 *       tools/cyc_watch/bsnes_cycles_probe.c -o tools/cyc_watch/bsnes_cycles_probe.exe
 * Run:
 *   bsnes_cycles_probe.exe <bsnes_libretro.dll> <rom.sfc>
 */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "libretro.h"

static void *load(HMODULE m, const char *n) {
    void *p = (void *)GetProcAddress(m, n);
    if (!p) { fprintf(stderr, "missing export: %s\n", n); exit(2); }
    return p;
}

/* minimal environment callback: accept pixel format, refuse the rest. */
static bool RETRO_CALLCONV env_cb(unsigned cmd, void *data) {
    switch (cmd) {
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: return true;
        case RETRO_ENVIRONMENT_GET_CAN_DUPE:
            if (data) *(bool *)data = true; return true;
        default: return false;
    }
}
static void RETRO_CALLCONV video_cb(const void *d, unsigned w, unsigned h, size_t p) {
    (void)d;(void)w;(void)h;(void)p;
}
static void RETRO_CALLCONV audio_cb(int16_t l, int16_t r) { (void)l;(void)r; }
static size_t RETRO_CALLCONV audio_batch_cb(const int16_t *d, size_t f) { (void)d; return f; }
static void RETRO_CALLCONV input_poll_cb(void) {}
static int16_t RETRO_CALLCONV input_state_cb(unsigned a, unsigned b, unsigned c, unsigned d) {
    (void)a;(void)b;(void)c;(void)d; return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <core.dll> <rom>\n", argv[0]); return 1; }
    HMODULE core = LoadLibraryA(argv[1]);
    if (!core) { fprintf(stderr, "LoadLibrary failed: %s (err %lu)\n", argv[1], GetLastError()); return 2; }

    void (*p_set_environment)(retro_environment_t)   = load(core, "retro_set_environment");
    void (*p_set_video)(retro_video_refresh_t)       = load(core, "retro_set_video_refresh");
    void (*p_set_audio)(retro_audio_sample_t)        = load(core, "retro_set_audio_sample");
    void (*p_set_audio_b)(retro_audio_sample_batch_t)= load(core, "retro_set_audio_sample_batch");
    void (*p_set_poll)(retro_input_poll_t)           = load(core, "retro_set_input_poll");
    void (*p_set_state)(retro_input_state_t)         = load(core, "retro_set_input_state");
    void (*p_init)(void)                             = load(core, "retro_init");
    bool (*p_load)(const struct retro_game_info *)   = load(core, "retro_load_game");
    void (*p_run)(void)                              = load(core, "retro_run");
    void (*p_reset)(void)                            = load(core, "retro_reset");
    uint64_t (*p_cycles)(void)                       = load(core, "bsnes_total_guest_cycles");
    void (*p_cyc_reset)(void)                        = load(core, "bsnes_reset_guest_cycles");

    p_set_environment(env_cb);
    p_set_video(video_cb);
    p_set_audio(audio_cb);
    p_set_audio_b(audio_batch_cb);
    p_set_poll(input_poll_cb);
    p_set_state(input_state_cb);
    p_init();

    /* load ROM bytes */
    FILE *f = fopen(argv[2], "rb");
    if (!f) { fprintf(stderr, "open rom failed: %s\n", argv[2]); return 2; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    void *buf = malloc(sz);
    if (fread(buf, 1, sz, f) != (size_t)sz) { fprintf(stderr, "rom read short\n"); return 2; }
    fclose(f);

    struct retro_game_info gi; memset(&gi, 0, sizeof gi);
    gi.path = argv[2]; gi.data = buf; gi.size = (size_t)sz; gi.meta = NULL;
    if (!p_load(&gi)) { fprintf(stderr, "retro_load_game failed\n"); return 3; }

    p_reset();
    p_cyc_reset();
    uint64_t c0 = p_cycles();
    printf("after reset: cycles=%llu\n", (unsigned long long)c0);

    uint64_t prev = c0;
    for (int frame = 1; frame <= 60; frame++) {
        p_run();
        uint64_t now = p_cycles();
        if (frame <= 3 || frame == 60) {
            printf("frame %2d: total=%llu  delta=%llu\n",
                   frame, (unsigned long long)now, (unsigned long long)(now - prev));
        }
        prev = now;
    }
    uint64_t total = p_cycles() - c0;
    double per_frame = total / 60.0;
    printf("\n60 frames: total=%llu  avg/frame=%.1f master cyc\n",
           (unsigned long long)total, per_frame);
    /* NTSC frame is ~357366 master clocks; accept a generous band. */
    int ok = (per_frame > 350000.0 && per_frame < 360000.0);
    printf("RESULT: %s (expected ~357366/frame NTSC)\n", ok ? "PASS" : "FAIL");
    FreeLibrary(core);
    return ok ? 0 : 1;
}
