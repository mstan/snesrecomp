/*
 * cosim_state.c -- full guest-architectural-state canonical hash (SNES_COSIM.md).
 * Shared by the recomp `snes-cosim` build and the `snes-cosim-ref` build; the
 * two sides run IDENTICAL hashing code so any divergence is a real guest one.
 * DEV/DIAGNOSTICS ONLY (see cosim_state.h).
 */
#ifdef SNES_COSIM
#include "cosim_state.h"
#include <string.h>
#include <stdio.h>

#include "snes/snes.h"
#include "snes/apu.h"
#include "snes/dsp.h"
#include "snes/spc.h"
#include "snes/ppu.h"
#include "snes/dma.h"
#include "snes/cart.h"
#include "snes/saveload.h"

/* Both builds provide the device machine + WRAM under these symbols. */
extern Snes    *g_snes;
extern uint8_t  g_ram[0x20000];

#ifdef SNES_COSIM_REF
/* B side: the reference 65816 is interp816, driven by the ref driver, which
 * also maintains the region-weighted master/bus cycle accumulators. */
#include "snes/interp816.h"
extern Interp816 *g_ref_cpu;
extern uint64_t   g_ref_master_cycles;
extern uint64_t   g_ref_cycles;
#else
/* A side: the recompiled 65816 keeps its state in g_cpu; master/bus cycles are
 * already accumulated per-block by the generated C (Axis-2). */
#include "cpu_state.h"
extern CpuState g_cpu;
extern uint64_t g_apu_pace_cycles_estimate;
extern uint64_t g_apu_last_sync_master;
extern uint64_t g_main_cpu_cycles_estimate;
extern uint8_t  g_memsel;
#endif

const char *const cosim_sub_names[COSIM_SUB_COUNT] = {
    "cpu", "ram", "apu", "ppu", "dma", "cart", "sio", "dsp", "spc", "pace"
};

/* ── FNV-1a 64 ──────────────────────────────────────────────────────────── */
#define FNV_OFF 1469598103934665603ULL
#define FNV_PRM 1099511628211ULL

static uint64_t fnv_bytes(uint64_t h, const void *data, size_t n) {
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= FNV_PRM; }
    return h;
}

/* A SaveLoadInfo that folds every serialized scalar/blob into an FNV hash,
 * so each subsystem's existing *_saveload walker doubles as a canonical
 * hasher. Because both builds use the SAME saveload sources, the byte stream
 * (and thus the hash) is identical for identical state. */
typedef struct { SaveLoadInfo base; uint64_t h; } HashSli;
static void hashsli_func(SaveLoadInfo *sli, void *data, size_t n) {
    HashSli *hs = (HashSli *)sli;
    hs->h = fnv_bytes(hs->h, data, n);
}
static void hashsli_init(HashSli *hs) { hs->base.func = hashsli_func; hs->h = FNV_OFF; }

/* ── canonical CPU emit (fixed order/width, no PC) ──────────────────────── */
static uint64_t hash_cpu(uint32_t *leader_pc_out) {
    uint16_t A, X, Y, S, D;
    uint8_t  DB, PB, P, E;
    uint32_t pc24;
#ifdef SNES_COSIM_REF
    Interp816 *c = g_ref_cpu;
    A = c->a; X = c->x; Y = c->y; S = c->sp; D = c->dp; DB = c->db; PB = c->k;
    P = interp816_getFlags(c); E = c->e ? 1 : 0;
    pc24 = ((uint32_t)c->k << 16) | c->pc;   /* ref keeps PC live+reliable */
#else
    CpuState *c = &g_cpu;
    A = c->A; X = c->X; Y = c->Y; S = c->S; D = c->D; DB = c->DB; PB = c->PB;
    P = c->P; E = c->emulation ? 1 : 0;
    pc24 = 0;   /* recomp has no live PC (label only; regen cosim_block sets it) */
#endif
    if (leader_pc_out) *leader_pc_out = pc24;
    /* Explicit little-endian, fixed field order. PC deliberately EXCLUDED
     * (currency mismatch — SNES_COSIM.md); reported separately as a label. */
    uint8_t b[12];
    b[0]=A&0xff; b[1]=A>>8; b[2]=X&0xff; b[3]=X>>8; b[4]=Y&0xff; b[5]=Y>>8;
    b[6]=S&0xff; b[7]=S>>8; b[8]=D&0xff; b[9]=D>>8; b[10]=DB; b[11]=PB;
    uint64_t h = fnv_bytes(FNV_OFF, b, sizeof b);
    h = fnv_bytes(h, &P, 1);
    h = fnv_bytes(h, &E, 1);
    return h;
}

/* ── recomp-only synthetic pacing (reported, never compared) ────────────── */
static uint64_t hash_pace(void) {
#ifdef SNES_COSIM_REF
    return FNV_OFF;   /* ref has no synthetic pacing */
#else
    uint64_t h = FNV_OFF;
    h = fnv_bytes(h, &g_apu_pace_cycles_estimate, 8);
    h = fnv_bytes(h, &g_apu_last_sync_master, 8);
    h = fnv_bytes(h, &g_main_cpu_cycles_estimate, 8);
    h = fnv_bytes(h, &g_memsel, 1);
    return h;
#endif
}

void cosim_state_snapshot(CosimSnapshot *out) {
    Snes *s = g_snes;
    memset(out, 0, sizeof *out);

    HashSli hs;
    out->sub[COSIM_SUB_CPU]  = hash_cpu(&out->last_leader_pc);
    out->sub[COSIM_SUB_RAM]  = fnv_bytes(FNV_OFF, g_ram, 0x20000);
    hashsli_init(&hs); apu_saveload (s->apu,  &hs.base); out->sub[COSIM_SUB_APU]  = hs.h;
    hashsli_init(&hs); ppu_saveload (s->ppu,  &hs.base); out->sub[COSIM_SUB_PPU]  = hs.h;
    hashsli_init(&hs); dma_saveload (s->dma,  &hs.base); out->sub[COSIM_SUB_DMA]  = hs.h;
    hashsli_init(&hs); cart_saveload(s->cart, &hs.base); out->sub[COSIM_SUB_CART] = hs.h;
    /* SIO: the snes struct blob (hPos..divideResult) + ramAdr. Mirrors the
     * exact region snes_saveload serializes (minus device sub-walks + WRAM). */
    hashsli_init(&hs);
    hs.base.func(&hs.base, &s->hPos, sizeof(*s) - offsetof(Snes, hPos));
    hs.base.func(&hs.base, &s->ramAdr, sizeof s->ramAdr);
    out->sub[COSIM_SUB_SIO] = hs.h;
    /* diagnostic-only sub-hashes */
    hashsli_init(&hs); dsp_saveload(s->apu->dsp, &hs.base); out->sub[COSIM_SUB_DSP] = hs.h;
    hashsli_init(&hs); spc_saveload(s->apu->spc, &hs.base); out->sub[COSIM_SUB_SPC] = hs.h;
    out->sub[COSIM_SUB_PACE] = hash_pace();

    /* combined = fold of the COMPARED subs only */
    uint64_t h = FNV_OFF;
    for (int i = 0; i < COSIM_SUB_COMPARED; i++)
        h = fnv_bytes(h, &out->sub[i], sizeof out->sub[i]);
    out->combined = h;

#ifdef SNES_COSIM_REF
    out->cycles = g_ref_cycles;
    out->master_cycles = g_ref_master_cycles;
#else
    out->cycles = g_cpu.cycles;
    out->master_cycles = g_cpu.master_cycles;
#endif
}

uint64_t cosim_state_ruler(void) {
#ifdef SNES_COSIM_REF
    return g_ref_master_cycles;
#else
    return g_cpu.master_cycles;
#endif
}

/* ── raw WRAM dump (byte-level localization of a `ram` hash split) ─────────
 * Writes the full 128 KiB g_ram image. Read-only => safe while parked. Diff
 * two parked sides' dumps with `cmp -l a.bin b.bin` to name the exact bytes
 * behind a frame-hash divergence that names only `ram`. */
int cosim_state_dump_ram(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return 3;
    size_t n = fwrite(g_ram, 1, 0x20000, f);
    fclose(f);
    return n == 0x20000 ? 0 : 4;
}

/* ── framebuffer dump (visual repro of a rendering divergence, headless) ──
 * Writes the CURRENT contents of the PPU render buffer (filled by the last
 * draw_ppu_frame; BGRA) as a 24-bit BMP. Deliberately does NOT re-render:
 * ppu_runLine mutates hashed PPU state, which would perturb the compared
 * hash on whichever side dumped. Read-only => safe while parked. */
int cosim_state_dump_fb(const char *path) {
    Ppu *p = g_snes->ppu;
    if (!p || !p->renderBuffer) return 2;   /* no render binding */
    FILE *f = fopen(path, "wb");
    if (!f) return 3;                       /* path unwritable */
    int w = 256, h = 224;
    int row_bytes = w * 3, pad = (4 - (row_bytes % 4)) % 4;
    int stride = row_bytes + pad, img_size = stride * h, file_size = 54 + img_size;
    uint8_t hdr[54]; memset(hdr, 0, sizeof hdr);
    hdr[0]='B'; hdr[1]='M';
    hdr[2]=(uint8_t)file_size; hdr[3]=(uint8_t)(file_size>>8);
    hdr[4]=(uint8_t)(file_size>>16); hdr[5]=(uint8_t)(file_size>>24);
    hdr[10]=54; hdr[14]=40;
    hdr[18]=(uint8_t)w; hdr[19]=(uint8_t)(w>>8);
    int neg_h = -h; memcpy(&hdr[22], &neg_h, 4);
    hdr[26]=1; hdr[28]=24;
    hdr[34]=(uint8_t)img_size; hdr[35]=(uint8_t)(img_size>>8);
    hdr[36]=(uint8_t)(img_size>>16); hdr[37]=(uint8_t)(img_size>>24);
    fwrite(hdr, 1, 54, f);
    uint8_t row[256 * 3 + 4]; memset(row, 0, sizeof row);
    for (int y = 0; y < h; y++) {
        const uint8_t *src = p->renderBuffer + (size_t)y * p->renderPitch;
        for (int x = 0; x < w; x++) {
            row[x*3+0] = src[x*4+0];
            row[x*3+1] = src[x*4+1];
            row[x*3+2] = src[x*4+2];
        }
        fwrite(row, 1, (size_t)stride, f);
    }
    fclose(f);
    return 0;
}

/* ── gate-3 fault injection ─────────────────────────────────────────────── */
int cosim_state_inject_ram(uint32_t addr, uint8_t val) {
    if (addr >= 0x20000) return 1;
    g_ram[addr] = val;
    return 0;
}

int cosim_state_inject_reg(const char *reg, uint32_t val) {
#ifdef SNES_COSIM_REF
    Interp816 *c = g_ref_cpu;
    if      (!strcmp(reg, "A"))  c->a = (uint16_t)val;
    else if (!strcmp(reg, "X"))  c->x = (uint16_t)val;
    else if (!strcmp(reg, "Y"))  c->y = (uint16_t)val;
    else if (!strcmp(reg, "S"))  c->sp = (uint16_t)val;
    else if (!strcmp(reg, "D"))  c->dp = (uint16_t)val;
    else if (!strcmp(reg, "DB")) c->db = (uint8_t)val;
    else if (!strcmp(reg, "PB")) c->k = (uint8_t)val;
    else if (!strcmp(reg, "P"))  interp816_setFlags(c, (uint8_t)val);
    else return 1;
#else
    CpuState *c = &g_cpu;
    if      (!strcmp(reg, "A"))  c->A = (uint16_t)val;
    else if (!strcmp(reg, "X"))  c->X = (uint16_t)val;
    else if (!strcmp(reg, "Y"))  c->Y = (uint16_t)val;
    else if (!strcmp(reg, "S"))  c->S = (uint16_t)val;
    else if (!strcmp(reg, "D"))  c->D = (uint16_t)val;
    else if (!strcmp(reg, "DB")) c->DB = (uint8_t)val;
    else if (!strcmp(reg, "PB")) c->PB = (uint8_t)val;
    else if (!strcmp(reg, "P"))  c->P = (uint8_t)val;
    else return 1;
#endif
    return 0;
}

/* ── field dumps (space-separated key=hex) ──────────────────────────────── */
void cosim_state_dump_cpu(char *buf, size_t n) {
    uint16_t A, X, Y, S, D; uint8_t DB, PB, P, E; uint32_t pc24;
#ifdef SNES_COSIM_REF
    Interp816 *c = g_ref_cpu;
    A=c->a; X=c->x; Y=c->y; S=c->sp; D=c->dp; DB=c->db; PB=c->k;
    P=interp816_getFlags(c); E=c->e?1:0; pc24=((uint32_t)c->k<<16)|c->pc;
    uint64_t cyc=g_ref_cycles, mcyc=g_ref_master_cycles;
#else
    CpuState *c=&g_cpu;
    A=c->A; X=c->X; Y=c->Y; S=c->S; D=c->D; DB=c->DB; PB=c->PB;
    P=c->P; E=c->emulation?1:0; pc24=0;
    uint64_t cyc=g_cpu.cycles, mcyc=g_cpu.master_cycles;
#endif
    snprintf(buf, n,
        "A=%04X X=%04X Y=%04X S=%04X D=%04X DB=%02X PB=%02X P=%02X E=%X "
        "PC=%06X cyc=%llu mcyc=%llu",
        A,X,Y,S,D,DB,PB,P,E,pc24,
        (unsigned long long)cyc,(unsigned long long)mcyc);
}

void cosim_state_dump_dev(char *buf, size_t n) {
    Snes *s = g_snes; Apu *a = s->apu;
    snprintf(buf, n,
        "hPos=%04X vPos=%04X inNmi=%d inIrq=%d inVbl=%d nmiEn=%d hIrqEn=%d "
        "vIrqEn=%d hTimer=%04X vTimer=%04X apuCatchup=%.3f samples=%u "
        "inPorts=%02X%02X%02X%02X outPorts=%02X%02X%02X%02X apuCycLeft=%02X"
#ifndef SNES_COSIM_REF
        " pace_apu=%llu pace_sync=%llu memsel=%02X"
#endif
        ,
        s->hPos, s->vPos, s->inNmi, s->inIrq, s->inVblank, s->nmiEnabled,
        s->hIrqEnabled, s->vIrqEnabled, s->hTimer, s->vTimer, s->apuCatchupCycles,
        (unsigned)a->dsp->sampleWrite,
        a->inPorts[0],a->inPorts[1],a->inPorts[2],a->inPorts[3],
        a->outPorts[0],a->outPorts[1],a->outPorts[2],a->outPorts[3],
        a->cpuCyclesLeft
#ifndef SNES_COSIM_REF
        ,(unsigned long long)g_apu_pace_cycles_estimate,
        (unsigned long long)g_apu_last_sync_master, g_memsel
#endif
        );
}

#endif /* SNES_COSIM */
