/*
 * ref_driver.c -- Track A B-side reference (SNES_COSIM.md): a headless,
 * deterministic SNES driven by the interp816 65816 interpreter over the
 * runner's OWN device sources (identical struct layouts => the cosim state
 * hash compares directly against the recomp A-side). Accurate H/V timing +
 * accurate APU stepping (the interp advances the real SPC at the true rate,
 * unlike the recomp's synthetic pacing) — so the audio off-cue surfaces as an
 * apu/dsp sub-hash split at a frame boundary.
 *
 * Built as `smw_cosim_ref` with SNES_COSIM + SNES_COSIM_REF. Dev/diagnostics.
 *
 * Frame loop ported from the LakeSnes H/V driver (SuperMarioWorldRecomp-oracle
 * snes.c snes_handle_pos_stuff), adapted to the runner's device funcs + NMI
 * delivery to interp816. Does NOT call ppu_runLine (the headless A-side never
 * renders either — PPU state on both sides is driven by register writes).
 */
#ifdef SNES_COSIM
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#include "snes/snes.h"
#include "snes/apu.h"
#include "snes/dsp.h"
#include "snes/dsp_shadow.h"
#include "snes/spc.h"
#include "snes/ppu.h"
#include "snes/dma.h"
#include "snes/cart.h"
#include "snes/interp816.h"
#include "types.h"
#include "cosim.h"

/* ── globals the runner device layer + cosim_state (REF) reference ────────── */
uint8_t    g_ram[0x20000];
Snes      *g_snes;
Ppu       *g_ppu;
Interp816 *g_ref_cpu;
uint64_t   g_ref_master_cycles;
uint64_t   g_ref_cycles;

/* ── RTL glue the runner device sources call ─────────────────────────────── */
void RtlApuLock(void)   {}
void RtlApuUnlock(void) {}
/* Accurate reference: latch the CPU->APU port immediately (hardware behaviour),
 * NOT the recomp's deferred sample-time scheduler. adr is $2140-$2143. */
void RtlApuWrite(uint16 adr, uint8 val) { g_snes->apu->inPorts[adr & 3] = val; }
/* The ref advances the APU in its own frame loop (accurate rate), so the
 * runner's per-touch catch-up accumulator is a no-op here. */
void rtl_accumulate_apu_catchup(void) {}
void NORETURN Die(const char *e) { fprintf(stderr, "ref FATAL: %s\n", e ? e : "(null)"); exit(1); }
void debug_on_wram_write_byte(uint32_t a, uint8_t o, uint8_t n) { (void)a;(void)o;(void)n; }
void debug_on_wram_write_word(uint32_t a, uint16_t o, uint16_t n) { (void)a;(void)o;(void)n; }

/* Globals the device sources reference (normally owned by main.c / infra). */
bool g_new_ppu = true;
bool g_fail = false;

/* Observability / enhancement hooks the device sources call — no-ops in the ref
 * (not linking ppu_dma_trace.c / dsp_shadow.c / interp_bridge.c). */
void ppudma_record_dma(int ch, int fromB, uint8_t aBank, uint16_t aAdr,
                       uint8_t bAdr, uint16_t size) {
    (void)ch;(void)fromB;(void)aBank;(void)aAdr;(void)bAdr;(void)size;
}
/* interp816 BRK dispatch: 0 = continue (no bridge in the pure-interp ref). */
int interp816_opcode_hook(uint32_t addr) { (void)addr; return 0; }
/* DSP cubic-audio shadow (opt-in, off): keep canon dry mix unchanged. */
DspShadow *dsp_shadow_create(void) { return NULL; }
void dsp_shadow_free(DspShadow *sh) { (void)sh; }
void dsp_shadow_process(DspShadow *sh, Dsp *dsp, int cL, int cR, int *oL, int *oR) {
    (void)sh;(void)dsp; *oL = cL; *oR = cR;
}
void dsp_shadow_verify_brr(const uint8_t *aram, uint16_t bs, int a, int b, const int16_t *c) {
    (void)aram;(void)bs;(void)a;(void)b;(void)c;
}
void dsp_shadow_verify_echo(const int16_t *l, const int16_t *r, const int8_t *co,
                            int idx, int sL, int sR) {
    (void)l;(void)r;(void)co;(void)idx;(void)sL;(void)sR;
}

/* ── interp816 memory bus = the runner's self-contained SNES bus ─────────── */
static uint8_t bus_read(void *mem, uint32_t adr)             { (void)mem; return snes_read(g_snes, adr); }
static void    bus_write(void *mem, uint32_t adr, uint8_t v) { (void)mem; snes_write(g_snes, adr, v); }

/* SPC cycles per master clock (LakeSnes: (32040*32)/(1364*262*60)). */
static const double kApuCyclesPerMaster = (32040.0 * 32.0) / (1364.0 * 262.0 * 60.0);

/* ── accurate H/V position driver (ported from LakeSnes handle_pos_stuff) ── */
static uint64_t s_frames;   /* completed frames (ref has no snes->frames) */

static void handle_pos_stuff(Snes *snes) {
    Interp816 *cpu = g_ref_cpu;
    /* H/V timer IRQ */
    if (snes->vIrqEnabled && snes->hIrqEnabled) {
        if (snes->vPos == (snes->vTimer + 1) && snes->hPos == (4 * snes->hTimer)) {
            snes->inIrq = true; cpu->irqWanted = true;
        }
    } else if (snes->vIrqEnabled && !snes->hIrqEnabled) {
        if (snes->vPos == (snes->vTimer + 1) && snes->hPos == 1024) {
            snes->inIrq = true; cpu->irqWanted = true;
        }
    } else if (!snes->vIrqEnabled && snes->hIrqEnabled) {
        if (snes->hPos == (4 * snes->hTimer)) { snes->inIrq = true; cpu->irqWanted = true; }
    }

    if (snes->hPos == 0) {
        bool startingVblank = false;
        if (snes->vPos == 0) {
            snes->inVblank = false; snes->inNmi = false;
            /* re-arm HDMA for the new frame */
            dma_startDma(snes->dma, 0, true);
        } else if (snes->vPos == 225) {
            startingVblank = !ppu_checkOverscan(g_ppu);
        } else if (snes->vPos == 240) {
            if (!snes->inVblank) startingVblank = true;
        }
        if (startingVblank) {
            ppu_handleVblank(g_ppu);
            snes->inVblank = true;
            snes->inNmi = true;
            if (snes->nmiEnabled) cpu->nmiWanted = true;   /* deliver NMI */
            if (snes->autoJoyRead) snes->autoJoyTimer = 0;
        }
    } else if (snes->hPos == 1024) {
        if (!snes->inVblank) dma_cycle(snes->dma);          /* per-line HDMA */
    }

    snes->hPos += 2;
    if (snes->hPos == 1364) {
        snes->hPos = 0;
        snes->vPos++;
        if (snes->vPos == 262) { snes->vPos = 0; s_frames++; }
    }
}

/* ── one guest frame: interp opcodes interleaved with H/V + accurate APU ─── */
static void run_one_frame(void) {
    Snes *snes = g_snes;
    Interp816 *cpu = g_ref_cpu;
    uint64_t target = s_frames + 1;
    /* Guard against a runaway (spin with no vPos progress): cap opcodes/frame. */
    long guard = 20000000;
    while (s_frames < target && guard-- > 0) {
        /* Instruction-granular co-sim checkpoint (SNES_COSIM_SYNC_PC): the ref's
         * live interp IS g_ref_cpu, which cosim_state snapshots directly, so no
         * sync needed. Offer this opcode boundary before executing it. */
        cosim_insn(((uint32_t)cpu->k << 16) | (uint32_t)cpu->pc);
        int cyc = interp816_runOpcode(cpu);         /* CPU bus cycles */
        if (cyc <= 0) cyc = 1;
        int master = cyc * 8;                        /* slowROM approx (6/8/12); */
        g_ref_cycles += (uint64_t)cyc;               /* reported only, not compared */
        g_ref_master_cycles += (uint64_t)master;
        for (int i = 0; i < master; i += 2) handle_pos_stuff(snes);
        /* accurate APU: advance the real SPC at the true rate */
        snes->apuCatchupCycles += (double)master * kApuCyclesPerMaster;
        snes_catchupApu(snes);
    }
}

static uint8_t *read_file(const char *path, uint32_t *size_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    uint8_t *b = (uint8_t *)malloc((size_t)n);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    if (b) *size_out = (uint32_t)n;
    return b;
}

int main(int argc, char **argv) {
    const char *rom = (argc > 1) ? argv[1] : "smw.sfc";
    uint32_t size = 0;
    uint8_t *data = read_file(rom, &size);
    if (!data) { fprintf(stderr, "ref: cannot read ROM '%s'\n", rom); return 1; }

    g_snes = snes_init(g_ram);
    g_ppu = g_snes->ppu;
    if (!snes_loadRom(g_snes, data, (int)size)) { fprintf(stderr, "ref: loadRom failed\n"); return 1; }
    snes_reset(g_snes, true);

    /* interp816 drives the bus; reset reads the reset vector via bus_read. */
    g_ref_cpu = interp816_init(NULL, bus_read, bus_write);
    interp816_reset(g_ref_cpu);

    fprintf(stderr, "ref: interp816 + runner devices, headless attract\n");
    cosim_init();                 /* connect the coordinator before frame 1 */
    for (;;) {
        run_one_frame();
        /* Deterministic audio consumer: drain one frame's worth so the DSP ring
         * keeps flowing (else it fills to DSP_SAMPLE_RING and all further samples
         * drop — the ref would look silent). Matches the A-side consumer rate so
         * both produce audio at the SNES native 32040/60.0988 = 533.12/frame. */
        {
            static double acc = 0.0; static int16_t buf[1024 * 2];
            acc += 32040.0 / 60.0988;
            int want = (int)acc; acc -= (double)want;
            while (want > 0) {
                int c = want > 1024 ? 1024 : want;
                dsp_getSamples(g_snes->apu->dsp, buf, c);
                want -= c;
            }
        }
        cosim_frame();
    }
    return 0;
}

#endif /* SNES_COSIM */
