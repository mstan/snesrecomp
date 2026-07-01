/*
 * harness_smw.c -- headless, deterministic SMW entry for the differential
 * co-simulation A-side (SNES_COSIM.md). Replaces the SDL/OpenGL main.c: NO
 * window, NO host audio sink, NO worker threads — the Gate-1 determinism
 * requirement, satisfied by construction. Boots SMW to attract (no input) and
 * loops RtlRunFrame(0); the cosim engine (cosim_init/cosim_frame, hooked inside
 * RtlRunFrame) drives the checkpoint lockstep with the coordinator.
 *
 * DEV/DIAGNOSTICS ONLY (built only under SNES_COSIM).
 *
 * Usage: harness_smw <rom.sfc>   (env SNES_COSIM_PORT / SNES_COSIM_STRIDE)
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "common_rtl.h"
#include "common_cpu_infra.h"
#include "snes/snes.h"
#include "spc_player.h"

/* Provided by the SMW game sources linked alongside this harness. */
extern const RtlGameInfo kSmwGameInfo;
struct SpcPlayer *SmwSpcPlayer_Create(void);

/* main.c normally defines this global; the runner references it extern. Here
 * the harness owns it. */
struct SpcPlayer *g_spc_player;

static uint8_t *read_file(const char *path, uint32_t *size_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    uint8_t *buf = (uint8_t *)malloc((size_t)n);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    if (buf) *size_out = (uint32_t)n;
    return buf;
}

int main(int argc, char **argv) {
    const char *rom = (argc > 1) ? argv[1] : "smw.sfc";
    uint32_t size = 0;
    uint8_t *data = read_file(rom, &size);
    if (!data) { fprintf(stderr, "cosim-harness: cannot read ROM '%s'\n", rom); return 1; }

    RtlRegisterGame(&kSmwGameInfo);
    Snes *snes = SnesInit(data, (int)size);
    if (!snes) { fprintf(stderr, "cosim-harness: SnesInit failed\n"); return 1; }

    g_spc_player = SmwSpcPlayer_Create();
    g_spc_player->initialize(g_spc_player);

    fprintf(stderr, "cosim-harness: SMW booted headless; running attract (no input)\n");
    for (;;) {
        /* attract/demo needs no controller input; identical inputs by
         * construction (Gate-1 determinism). cosim_init() connects the
         * coordinator on the first frame; cosim_frame() checkpoints. */
        RtlRunFrame(0);
    }
    return 0;
}
