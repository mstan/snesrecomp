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
#include <string.h>

#include "snes/snes.h"
#include "snes/apu.h"
#include "snes/dsp.h"
#include "snes/dsp_shadow.h"
#include "snes/spc.h"
#include "snes/ppu.h"
#include "snes/dma.h"
#include "snes/cart.h"
#include "snes/dsp1.h"
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
static uint64_t s_joy_reads;
static uint64_t s_joy_nonzero_reads;
static uint64_t s_joy_strobe_writes;

/* ── RTL glue the runner device sources call ─────────────────────────────── */
void RtlApuLock(void)   {}
void RtlApuUnlock(void) {}
void rtl_sync_apu_to_cpu_locked(void) {}
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
bool g_fail = false;
uint8 g_snesrecomp_last_hdmaen;

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
static uint8_t bus_read(void *mem, uint32_t adr) {
    (void)mem;
    uint8_t value = snes_read(g_snes, adr);
    uint16_t reg = (uint16_t)adr;
    uint8_t bank = (uint8_t)(adr >> 16);
    bool hardware_bank = bank < 0x40 || (bank >= 0x80 && bank < 0xc0);
    if (hardware_bank &&
        (reg == 0x4016 || reg == 0x4017 ||
         reg == 0x4218 || reg == 0x4219)) {
        s_joy_reads++;
        if (value) {
            if (!s_joy_nonzero_reads)
                fprintf(stderr,
                        "ref: first nonzero joypad read master=%llu reg=%04x value=%02x\n",
                        (unsigned long long)g_ref_master_cycles, reg, value);
            s_joy_nonzero_reads++;
        }
    }
    return value;
}
static void bus_write(void *mem, uint32_t adr, uint8_t v) {
    (void)mem;
    uint8_t bank = (uint8_t)(adr >> 16);
    if ((bank < 0x40 || (bank >= 0x80 && bank < 0xc0)) &&
        (uint16_t)adr == 0x4016)
        s_joy_strobe_writes++;
    snes_write(g_snes, adr, v);
}

/* SPC cycles per master clock (LakeSnes: (32040*32)/(1364*262*60)). */
static const double kApuCyclesPerMaster = (32040.0 * 32.0) / (1364.0 * 262.0 * 60.0);

/* ── accurate H/V position driver (ported from LakeSnes handle_pos_stuff) ── */
static uint64_t s_frames;   /* completed frames (ref has no snes->frames) */
static uint64_t s_cpu_insns;
static bool s_render_video;
static uint32_t s_pc_ring[32];
static uint32_t s_pc_seq;

typedef struct InputEvent {
    uint64_t start;
    uint64_t duration;
    uint16_t mask;
} InputEvent;

static InputEvent s_input_events[64];
static uint32_t s_input_event_count;

enum {
    kVideoWidth = 256,
    kVideoHeight = 224,
    kVideoPitch = kVideoWidth * 4,
};

static uint8_t s_video_pixels[kVideoPitch * kVideoHeight];
static uint8_t s_best_video_pixels[kVideoPitch * kVideoHeight];

typedef struct RefStats {
    uint64_t last_cpu_insns;
    uint64_t min_cpu_insns;
    uint64_t max_cpu_insns;
    uint64_t logic_hash;
    uint64_t logic_changes;
    uint64_t logic_frozen_run;
    uint64_t logic_frozen_max;
    uint64_t video_hash;
    uint64_t video_changes;
    uint64_t video_active_frames;
    uint64_t video_blank_run;
    uint64_t video_blank_max;
    uint64_t video_frozen_run;
    uint64_t video_frozen_max;
    uint32_t video_max_changed_pixels;
    uint64_t video_best_brightness;
    uint64_t audio_active_frames;
    uint64_t audio_silent_run;
    uint64_t audio_silent_max;
    uint64_t audio_underruns;
    uint32_t audio_peak;
} RefStats;

static RefStats s_stats;

static uint64_t hash_bytes(const void *data, size_t size) {
    const uint8_t *p = (const uint8_t *)data;
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < size; i++) {
        hash ^= p[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void update_max_run(uint64_t run, uint64_t *maximum) {
    if (run > *maximum) *maximum = run;
}

static void collect_logic_stats(void) {
    uint64_t frame_insns = s_cpu_insns - s_stats.last_cpu_insns;
    s_stats.last_cpu_insns = s_cpu_insns;
    if (!s_stats.min_cpu_insns || frame_insns < s_stats.min_cpu_insns)
        s_stats.min_cpu_insns = frame_insns;
    if (frame_insns > s_stats.max_cpu_insns)
        s_stats.max_cpu_insns = frame_insns;

    uint64_t hash = hash_bytes(g_ram, sizeof(g_ram));
    if (s_frames > 1 && hash != s_stats.logic_hash) {
        s_stats.logic_changes++;
        s_stats.logic_frozen_run = 0;
    } else if (s_frames > 1) {
        s_stats.logic_frozen_run++;
        update_max_run(s_stats.logic_frozen_run, &s_stats.logic_frozen_max);
    }
    s_stats.logic_hash = hash;
}

static void collect_video_stats(void) {
    if (!s_render_video) return;
    const uint32_t *pixels = (const uint32_t *)s_video_pixels;
    const size_t count = kVideoWidth * kVideoHeight;
    const uint32_t first = pixels[0] & 0x00ffffffu;
    uint32_t changed = 0;
    uint64_t brightness = 0;
    for (size_t i = 0; i < count; i++) {
        uint32_t pixel = pixels[i];
        brightness += ((pixel >> 16) & 0xffu) +
                      ((pixel >> 8) & 0xffu) +
                      (pixel & 0xffu);
        if (!i) continue;
        changed += ((pixels[i] & 0x00ffffffu) != first);
    }

    uint64_t hash = hash_bytes(s_video_pixels, sizeof(s_video_pixels));
    if (s_frames > 1 && hash != s_stats.video_hash) {
        s_stats.video_changes++;
        s_stats.video_frozen_run = 0;
    } else if (s_frames > 1) {
        s_stats.video_frozen_run++;
        update_max_run(s_stats.video_frozen_run, &s_stats.video_frozen_max);
    }
    s_stats.video_hash = hash;

    if (changed) {
        s_stats.video_active_frames++;
        s_stats.video_blank_run = 0;
        if (changed > s_stats.video_max_changed_pixels)
            s_stats.video_max_changed_pixels = changed;
        if (brightness > s_stats.video_best_brightness) {
            s_stats.video_best_brightness = brightness;
            memcpy(s_best_video_pixels, s_video_pixels, sizeof(s_video_pixels));
        }
    } else {
        s_stats.video_blank_run++;
        update_max_run(s_stats.video_blank_run, &s_stats.video_blank_max);
    }
}

static void collect_audio_stats(Dsp *dsp, uint32_t available) {
    uint32_t inspect = available < DSP_SAMPLE_RING ? available : DSP_SAMPLE_RING;
    bool active = false;
    for (uint32_t i = 0; i < inspect; i++) {
        uint32_t index = (dsp->sampleRead + i) & (DSP_SAMPLE_RING - 1);
        int left = dsp->sampleBuffer[index * 2];
        int right = dsp->sampleBuffer[index * 2 + 1];
        uint32_t left_abs = (uint32_t)(left < 0 ? -(int64_t)left : left);
        uint32_t right_abs = (uint32_t)(right < 0 ? -(int64_t)right : right);
        uint32_t peak = left_abs > right_abs ? left_abs : right_abs;
        if (peak) active = true;
        if (peak > s_stats.audio_peak) s_stats.audio_peak = peak;
    }
    if (active) {
        s_stats.audio_active_frames++;
        s_stats.audio_silent_run = 0;
    } else {
        s_stats.audio_silent_run++;
        update_max_run(s_stats.audio_silent_run, &s_stats.audio_silent_max);
    }
}

static bool write_ppm(const char *path, const uint8_t *source) {
    if (!path || !path[0]) return true;
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    fprintf(f, "P6\n%d %d\n255\n", kVideoWidth, kVideoHeight);
    const uint32_t *pixels = (const uint32_t *)source;
    for (size_t i = 0; i < (size_t)kVideoWidth * kVideoHeight; i++) {
        uint8_t rgb[3] = {
            (uint8_t)(pixels[i] >> 16),
            (uint8_t)(pixels[i] >> 8),
            (uint8_t)pixels[i],
        };
        if (fwrite(rgb, 1, sizeof(rgb), f) != sizeof(rgb)) {
            fclose(f);
            return false;
        }
    }
    return fclose(f) == 0;
}

static bool add_input_event(const char *text) {
    unsigned long long start = 0;
    unsigned long long duration = 0;
    unsigned mask = 0;
    char trailing = '\0';
    if (s_input_event_count >= 64 ||
        sscanf(text, "%llu:%llu:%x%c",
               &start, &duration, &mask, &trailing) != 3 ||
        !duration || mask > 0xffffu)
        return false;
    s_input_events[s_input_event_count++] = (InputEvent){
        (uint64_t)start, (uint64_t)duration, (uint16_t)mask
    };
    return true;
}

static void apply_frame_input(void) {
    static uint16_t previous;
    uint16_t input = 0;
    for (uint32_t i = 0; i < s_input_event_count; i++) {
        InputEvent *event = &s_input_events[i];
        if (s_frames >= event->start &&
            s_frames - event->start < event->duration)
            input |= event->mask;
    }
    if (input != previous) {
        fprintf(stderr, "ref: input frame=%llu mask=%03x\n",
                (unsigned long long)s_frames, input);
        previous = input;
    }
    g_snes->input1_currentState = input;
}

static void handle_pos_stuff(Snes *snes) {
    Interp816 *cpu = g_ref_cpu;
    if (snes->autoJoyTimer)
        snes->autoJoyTimer =
            snes->autoJoyTimer <= 2 ? 0 : (uint16_t)(snes->autoJoyTimer - 2);

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
        if (s_render_video && snes->vPos <= kVideoHeight)
            ppu_runLine(g_ppu, snes->vPos);
        if (snes->vPos == 0) {
            snes->inVblank = false; snes->inNmi = false;
            dma_initHdma(snes->dma);
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
            if (snes->autoJoyRead) snes->autoJoyTimer = 4224;
        }
    } else if (snes->hPos == 1024) {
        if (!snes->inVblank) dma_doHdma(snes->dma);
    }

    snes->hPos += 2;
    if (snes->hPos == 1364) {
        snes->hPos = 0;
        snes->vPos++;
        if (snes->vPos == 262) { snes->vPos = 0; s_frames++; }
    }
}

/* ── one guest frame: interp opcodes interleaved with H/V + accurate APU ─── */
static bool run_one_frame(void) {
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
        s_pc_ring[s_pc_seq++ & 31u] =
            ((uint32_t)cpu->k << 16) | (uint32_t)cpu->pc;
        int cyc = interp816_runOpcode(cpu);         /* CPU bus cycles */
        s_cpu_insns++;
        if (cyc <= 0) cyc = 1;
        int master = cyc * 8;                        /* slowROM approx (6/8/12); */
        g_ref_cycles += (uint64_t)cyc;               /* reported only, not compared */
        g_ref_master_cycles += (uint64_t)master;
        for (int i = 0; i < master; i += 2) handle_pos_stuff(snes);
        /* accurate APU: advance the real SPC at the true rate */
        snes->apuCatchupCycles += (double)master * kApuCyclesPerMaster;
        snes_catchupApu(snes);
    }
    return guard > 0;
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
    const char *frame_dump = NULL;
    const char *final_frame_dump = NULL;
    bool no_render = false;
    uint64_t standalone_frames = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
            standalone_frames = strtoull(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "--frame-dump") && i + 1 < argc) {
            frame_dump = argv[++i];
        } else if (!strcmp(argv[i], "--final-frame-dump") && i + 1 < argc) {
            final_frame_dump = argv[++i];
        } else if (!strcmp(argv[i], "--input") && i + 1 < argc) {
            if (!add_input_event(argv[++i])) {
                fprintf(stderr,
                        "ref: invalid input event; expected start:duration:hexmask\n");
                return 2;
            }
        } else if (!strcmp(argv[i], "--no-render")) {
            no_render = true;
        } else {
            fprintf(stderr,
                    "usage: %s [rom] [--frames count] [--frame-dump path] "
                    "[--final-frame-dump path] "
                    "[--input start:duration:hexmask] [--no-render]\n", argv[0]);
            return 2;
        }
    }
    s_render_video = standalone_frames != 0 && !no_render;
    uint32_t size = 0;
    uint8_t *data = read_file(rom, &size);
    if (!data) { fprintf(stderr, "ref: cannot read ROM '%s'\n", rom); return 1; }

    g_snes = snes_init(g_ram);
    cart_set_master_clock_source(g_snes->cart, &g_ref_master_cycles);
    g_ppu = g_snes->ppu;
    if (!snes_loadRom(g_snes, data, (int)size)) { fprintf(stderr, "ref: loadRom failed\n"); return 1; }
    snes_reset(g_snes, true);
    if (s_render_video)
        PpuBeginDrawing(g_ppu, s_video_pixels, kVideoPitch, 0);

    /* interp816 drives the bus; reset reads the reset vector via bus_read. */
    g_ref_cpu = interp816_init(NULL, bus_read, bus_write);
    interp816_reset(g_ref_cpu);

    fprintf(stderr, "ref: interp816 + runner devices, headless attract\n");
    if (!standalone_frames)
        cosim_init();             /* connect the coordinator before frame 1 */
    for (;;) {
        apply_frame_input();
        if (!run_one_frame()) {
            fprintf(stderr, "ref: opcode guard tripped at frame %llu\n",
                    (unsigned long long)s_frames);
            return 1;
        }
        collect_logic_stats();
        collect_video_stats();
        /* Deterministic audio consumer: drain one frame's worth so the DSP ring
         * keeps flowing (else it fills to DSP_SAMPLE_RING and all further samples
         * drop — the ref would look silent). Matches the A-side consumer rate so
         * both produce audio at the SNES native 32040/60.0988 = 533.12/frame. */
        {
            static int16_t buf[1024 * 2];
            /* Drain everything past a one-block cushion rather than a fixed
             * 533.12/frame. The engine produces exactly
             * RTL_APU_CYCLES_PER_FRAME/32 = 534 natives per frame, so retiring
             * the nominal 32040/60.0988 rate leaks 0.88 natives per frame. The
             * retired dsp_getSamples hid that by always retiring 534 regardless
             * of the count asked for; the exact-consume version does not, and
             * the ring would pin at DSP_SAMPLE_RING after ~9300 frames and then
             * silently drop ~0.16% of samples for the rest of the run, desyncing
             * the A/B audio stats on any long cosim. The A side absorbs the same
             * imbalance with its occupancy servo; draining to a fixed cushion is
             * simpler here and just as deterministic. */
            Dsp *dsp = g_snes->apu->dsp;
            uint32_t available = dsp->sampleWrite - dsp->sampleRead;
            collect_audio_stats(dsp, available);
            const uint32_t cushion = 534;
            if (available > cushion) {
                uint32_t want = available - cushion;
                while (want != 0) {
                    uint32_t chunk = want > 1024 ? 1024 : want;
                    dsp_getSamples(dsp, buf, (int)chunk);
                    want -= chunk;
                }
            } else {
                s_stats.audio_underruns++;
            }
        }
        if (standalone_frames) {
            if (s_frames >= standalone_frames) break;
        } else {
            cosim_frame();
        }
    }

    if (cart_has_dsp1(g_snes->cart)) {
        Dsp1 *dsp1 = g_snes->cart->dsp1;
        if (!dsp1_firmware_loaded(dsp1) && !dsp1_hle_active(dsp1)) {
            fprintf(stderr, "ref: DSP-1 cartridge has no usable backend\n");
            return 1;
        }
        if (dsp1_firmware_loaded(dsp1) &&
            dsp1_instructions_executed(dsp1) == 0) {
            fprintf(stderr, "ref: DSP-1 firmware executed zero instructions\n");
            return 1;
        }
        if (dsp1_hle_failed(dsp1)) {
            fprintf(stderr, "ref: DSP-1 HLE failed on command %02x\n",
                    dsp1_hle_failed_command(dsp1));
            return 1;
        }
        fprintf(stderr, "ref: dsp1_backend=%s\n",
                dsp1_firmware_loaded(dsp1) ? "lle" : "hle");
    }
    const uint8_t *representative = s_stats.video_best_brightness
        ? s_best_video_pixels : s_video_pixels;
    if (s_render_video && !write_ppm(frame_dump, representative)) {
        fprintf(stderr, "ref: cannot write frame dump '%s'\n", frame_dump);
        return 1;
    }
    if (s_render_video && !write_ppm(final_frame_dump, s_video_pixels)) {
        fprintf(stderr, "ref: cannot write final frame dump '%s'\n",
                final_frame_dump);
        return 1;
    }

    if (standalone_frames >= 120) {
        if (!s_stats.logic_changes) {
            fprintf(stderr, "ref: logic did not progress across frames\n");
            return 1;
        }
        if (g_snes->apu->dsp->sampleWrite < standalone_frames * 500 ||
            !s_stats.audio_active_frames) {
            fprintf(stderr, "ref: audio did not produce active continuous output\n");
            return 1;
        }
        if (s_render_video &&
            (!s_stats.video_active_frames || !s_stats.video_changes)) {
            fprintf(stderr, "ref: rendered video stayed blank or frozen\n");
            return 1;
        }
    }
    fprintf(stderr, "ref: final_pc=%02x:%04x pc_tail=",
            g_ref_cpu->k, g_ref_cpu->pc);
    uint32_t pc_count = s_pc_seq < 32 ? s_pc_seq : 32;
    uint32_t pc_first = s_pc_seq - pc_count;
    for (uint32_t i = 0; i < pc_count; i++) {
        uint32_t pc = s_pc_ring[(pc_first + i) & 31u];
        fprintf(stderr, "%s%02x:%04x", i ? "," : "",
                (unsigned)(pc >> 16), (unsigned)(pc & 0xffffu));
    }
    fputc('\n', stderr);
    fprintf(stderr,
            "ref: joy_reads=%llu joy_nonzero_reads=%llu joy_strobes=%llu\n",
            (unsigned long long)s_joy_reads,
            (unsigned long long)s_joy_nonzero_reads,
            (unsigned long long)s_joy_strobe_writes);
    if (g_snes->cart->dsp1) {
        bool first = true;
        fputs("ref: dsp1_commands=", stderr);
        for (unsigned command = 0; command < 256; command++) {
            uint64_t count =
                dsp1_command_count(g_snes->cart->dsp1, (uint8_t)command);
            if (!count) continue;
            fprintf(stderr, "%s%02x:%llu", first ? "" : ",", command,
                    (unsigned long long)count);
            first = false;
        }
        fputc('\n', stderr);
    }
    fprintf(stderr,
            "ref: PASS frames=%llu master=%llu cpu_insns=%llu "
            "logic_changes=%llu logic_frozen_max=%llu "
            "audio_samples=%u audio_active_frames=%llu audio_underruns=%llu "
            "audio_silent_max=%llu audio_peak=%u "
            "video_active_frames=%llu video_changes=%llu "
            "video_blank_max=%llu video_frozen_max=%llu "
            "video_hash=%016llx dsp1_insns=%llu "
            "dsp1_reads=%llu dsp1_writes=%llu\n",
            (unsigned long long)s_frames,
            (unsigned long long)g_ref_master_cycles,
            (unsigned long long)s_cpu_insns,
            (unsigned long long)s_stats.logic_changes,
            (unsigned long long)s_stats.logic_frozen_max,
            g_snes->apu->dsp->sampleWrite,
            (unsigned long long)s_stats.audio_active_frames,
            (unsigned long long)s_stats.audio_underruns,
            (unsigned long long)s_stats.audio_silent_max,
            s_stats.audio_peak,
            (unsigned long long)s_stats.video_active_frames,
            (unsigned long long)s_stats.video_changes,
            (unsigned long long)s_stats.video_blank_max,
            (unsigned long long)s_stats.video_frozen_max,
            (unsigned long long)s_stats.video_hash,
            (unsigned long long)(g_snes->cart->dsp1
                ? dsp1_instructions_executed(g_snes->cart->dsp1) : 0),
            (unsigned long long)(g_snes->cart->dsp1
                ? dsp1_host_reads(g_snes->cart->dsp1) : 0),
            (unsigned long long)(g_snes->cart->dsp1
                ? dsp1_host_writes(g_snes->cart->dsp1) : 0));
    return 0;
}

#endif /* SNES_COSIM */
