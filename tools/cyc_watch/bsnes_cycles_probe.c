/*
 * bsnes_cycles_probe -- exercise the bsnes Axis-2 cycle hooks (dev-only).
 *
 *   1) sanity: master-clock counter advances ~one NTSC frame (357368) per
 *      retro_run;
 *   2) REGION cross-check: with a two-anchor CPU-cycle latch (set via
 *      bsnes_set_cyc_anchor), report bsnes's CPU (bus+internal) cycle delta
 *      over a guest-PC region — the unit the recomp/authority model emits, so
 *      the numbers are directly comparable.
 *
 * Build (PowerShell / mingw):
 *   gcc -O2 -I F:/Projects/_bsnes_src/bsnes/target-libretro \
 *       tools/cyc_watch/bsnes_cycles_probe.c -o tools/cyc_watch/bsnes_cycles_probe.exe
 * Run:
 *   bsnes_cycles_probe.exe <core.dll> <rom.sfc> [startPC endPC expected]
 *   (PCs hex, e.g. 0x008000 0x008011)
 */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "libretro.h"

static void *req(HMODULE m, const char *n) {
    void *p = (void *)GetProcAddress(m, n);
    if (!p) { fprintf(stderr, "missing export: %s\n", n); exit(2); }
    return p;
}

static bool RETRO_CALLCONV env_cb(unsigned cmd, void *data) {
    switch (cmd) {
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: return true;
        case RETRO_ENVIRONMENT_GET_CAN_DUPE: if (data) *(bool *)data = true; return true;
        default: return false;
    }
}
static void RETRO_CALLCONV video_cb(const void *d, unsigned w, unsigned h, size_t p) { (void)d;(void)w;(void)h;(void)p; }
static void RETRO_CALLCONV audio_cb(int16_t l, int16_t r) { (void)l;(void)r; }
static size_t RETRO_CALLCONV audio_batch_cb(const int16_t *d, size_t f) { (void)d; return f; }
static void RETRO_CALLCONV input_poll_cb(void) {}
static int16_t RETRO_CALLCONV input_state_cb(unsigned a, unsigned b, unsigned c, unsigned d) { (void)a;(void)b;(void)c;(void)d; return 0; }

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <core.dll> <rom> [startPC endPC expected]\n", argv[0]); return 1; }
    HMODULE core = LoadLibraryA(argv[1]);
    if (!core) { fprintf(stderr, "LoadLibrary failed (err %lu)\n", GetLastError()); return 2; }

    void (*p_set_environment)(retro_environment_t)   = req(core, "retro_set_environment");
    void (*p_set_video)(retro_video_refresh_t)       = req(core, "retro_set_video_refresh");
    void (*p_set_audio)(retro_audio_sample_t)        = req(core, "retro_set_audio_sample");
    void (*p_set_audio_b)(retro_audio_sample_batch_t)= req(core, "retro_set_audio_sample_batch");
    void (*p_set_poll)(retro_input_poll_t)           = req(core, "retro_set_input_poll");
    void (*p_set_state)(retro_input_state_t)         = req(core, "retro_set_input_state");
    void (*p_init)(void)                             = req(core, "retro_init");
    bool (*p_load)(const struct retro_game_info *)   = req(core, "retro_load_game");
    void (*p_run)(void)                              = req(core, "retro_run");
    void (*p_reset)(void)                            = req(core, "retro_reset");
    uint64_t (*p_master)(void)                       = req(core, "bsnes_total_guest_cycles");
    uint64_t (*p_cpu)(void)                          = req(core, "bsnes_total_cpu_cycles");
    void (*p_set_anchor)(int, uint32_t)              = req(core, "bsnes_set_cyc_anchor");
    uint64_t (*p_anchor_cyc)(int)                    = req(core, "bsnes_get_anchor_cpu_cycles");
    int (*p_anchor_hit)(int)                         = req(core, "bsnes_anchor_hit");

    p_set_environment(env_cb); p_set_video(video_cb);
    p_set_audio(audio_cb); p_set_audio_b(audio_batch_cb);
    p_set_poll(input_poll_cb); p_set_state(input_state_cb);
    p_init();

    FILE *f = fopen(argv[2], "rb");
    if (!f) { fprintf(stderr, "open rom failed\n"); return 2; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    void *buf = malloc(sz);
    if (fread(buf, 1, sz, f) != (size_t)sz) { fprintf(stderr, "rom read short\n"); return 2; }
    fclose(f);
    struct retro_game_info gi; memset(&gi, 0, sizeof gi);
    gi.path = argv[2]; gi.data = buf; gi.size = (size_t)sz;
    if (!p_load(&gi)) { fprintf(stderr, "retro_load_game failed\n"); return 3; }

    int anchor_mode = (argc >= 5);
    uint32_t startPC = 0, endPC = 0; long expected = -1;
    if (anchor_mode) {
        startPC = (uint32_t)strtoul(argv[3], NULL, 0);
        endPC   = (uint32_t)strtoul(argv[4], NULL, 0);
        expected = (argc >= 6) ? strtol(argv[5], NULL, 0) : -1;
        p_set_anchor(0, startPC);
        p_set_anchor(1, endPC);
    }

    p_reset();                 /* retro_reset zeroes the counters + latches */

    if (anchor_mode) {
        for (int frame = 1; frame <= 10 && !(p_anchor_hit(0) && p_anchor_hit(1)); frame++)
            p_run();
        if (!p_anchor_hit(0) || !p_anchor_hit(1)) {
            printf("FAIL: anchors not both hit (start=%d end=%d)\n",
                   p_anchor_hit(0), p_anchor_hit(1));
            return 1;
        }
        uint64_t a0 = p_anchor_cyc(0), a1 = p_anchor_cyc(1);
        long long delta = (long long)a1 - (long long)a0;
        printf("REGION $%06X -> $%06X : bsnes CPU cycles = %lld  (latch0=%llu latch1=%llu)\n",
               startPC, endPC, delta, (unsigned long long)a0, (unsigned long long)a1);
        if (expected >= 0) {
            printf("authority predicts %ld CPU cycles -> %s\n", expected,
                   (delta == expected) ? "MATCH" : "MISMATCH");
            return (delta == expected) ? 0 : 1;
        }
        return 0;
    }

    /* frame sanity */
    uint64_t m0 = p_master(), c0 = p_cpu();
    for (int i = 0; i < 60; i++) p_run();
    double mpf = (p_master() - m0) / 60.0;
    printf("master/frame=%.1f (exp ~357368)  cpu cyc in 60f=%llu\n",
           mpf, (unsigned long long)(p_cpu() - c0));
    return (mpf > 350000.0 && mpf < 360000.0) ? 0 : 1;
}
