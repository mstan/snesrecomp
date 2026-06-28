/*
 * cpu_diff.c — 65816 instruction-semantics differential harness (Axis 1).
 *
 * For every opcode in g_ops[] (gen_ops.c, emitted by the REAL v2 recompiler),
 * set an identical randomized CPU state on both the recompiled function and the
 * interp816 reference interpreter (LakeSnes), run ONE opcode on each, and diff
 * the resulting registers + flags. A divergence is a recompiler codegen bug
 * (or, rarely, an interp816 bug — investigate either way). No ROM, no game:
 * a flat-RAM bus serves interp816's fetches and the emitted RTS epilogue's pops.
 *
 * RTS-frame convention: the emitted function ends in an RTS that pops a 2-byte
 * return frame off cpu->S (a real call would have pushed it). We run with
 * host_return_valid=0 and subtract 2 from cpu->S after the call to recover the
 * opcode's true effect on S (works for stack ops too: TXS sets S then RTS pops).
 *
 * Build/run: tests/cpu_diff/run.ps1 (mingw gcc on Windows).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common_cpu_infra.h"
#include "cpu_trace.h"
#include "interp816.h"
#include "cpu_diff.h"

/* ── SEPARATE buses so memory stores diff independently: the recomp writes RAM,
 * interp816 writes RAM2; both are restored from REF each iteration. WIN (banks
 * 0-1) is the working window the test addressing stays inside. ── */
#define MEMSZ 0x1000000u
#define WRAM  0x7E0000u   /* canonical low-WRAM base; both buses mirror $00/$7E here */
#define WSZ   0x2000u     /* low-WRAM mirror size (dp/abs tests stay inside) */
#define FAR   0xC00000u   /* far window base (banks $C0-$C1) for long addressing */
#define FSZ   0x20000u    /* far window size: long $C0:FFF0 + long,X bank-carry */
static uint8_t *RAM;    /* recomp bus */
static uint8_t *RAM2;   /* interp816 bus */
static uint8_t *REF;    /* reference WRAM pattern, copied into both each iter */
static uint8_t *REFF;   /* reference FAR pattern (long addressing) */

/* Canonicalize an address so the SNES low-WRAM mirror ($00-$3F/$80-$BF:0000-1FFF
 * and $7E:0000-1FFF) is ONE physical location — matching what the recomp does
 * (it routes dp/WRAM to $7E). Without this the recomp's $7E read and interp816's
 * $00 read would hit different bytes. Applied identically to both buses. */
static uint32_t mapa(uint32_t bank, uint32_t addr) {
    bank &= 0xFF; addr &= 0xFFFF;
    if (addr < 0x2000 && (bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF)))
        return WRAM + addr;
    return (bank << 16) | addr;
}
uint8 cpu_read8(CpuState *cpu, uint8 bank, uint16 addr) {
    (void)cpu; return RAM[mapa(bank, addr)];
}
/* The real cpu_read16/write16 (cpu_state.c) read PHYSICALLY-CONTIGUOUS bytes
 * (cpu->ram[off],[off+1] / RomPtr p[0],p[1]), i.e. the 24-bit address increments
 * across a bank boundary ($C0:FFFF -> $C1:0000) — it does NOT wrap within the
 * bank. Mirror that here so the harness tests the recomp's real word semantics. */
uint16 cpu_read16(CpuState *cpu, uint8 bank, uint16 addr) {
    (void)cpu; uint32_t a = ((uint32)bank << 16) | addr;
    return (uint16)RAM[mapa(a >> 16, a)] | ((uint16)RAM[mapa((a + 1) >> 16, a + 1)] << 8);
}
void cpu_write8(CpuState *cpu, uint8 bank, uint16 addr, uint8 v) {
    (void)cpu; RAM[mapa(bank, addr)] = v;
}
void cpu_write16(CpuState *cpu, uint8 bank, uint16 addr, uint16 v) {
    (void)cpu; uint32_t a = ((uint32)bank << 16) | addr;
    RAM[mapa(a >> 16, a)] = (uint8)v;
    RAM[mapa((a + 1) >> 16, a + 1)] = (uint8)(v >> 8);
}
static uint8_t i816_read(void *mem, uint32_t adr) {
    return ((uint8_t *)mem)[mapa(adr >> 16, adr)];
}
static void i816_write(void *mem, uint32_t adr, uint8_t v) {
    ((uint8_t *)mem)[mapa(adr >> 16, adr)] = v;
}

/* ── recomp runtime seam stubs (cpu_state.c / common_cpu_infra.c in prod) ── */
CpuState g_cpu;
const char *g_last_recomp_func;
int snes_frame_counter = 0;
int g_recomp_stack_top = 0;
uint16_t g_cpu_entry_s[256];
void RecompStackPush(const char *n) { (void)n; if (g_recomp_stack_top < 255) g_recomp_stack_top++; }
void RecompStackPop(void) { if (g_recomp_stack_top > 0) g_recomp_stack_top--; }
void WatchdogCheck(void) {}
int  cpu_resolve_ancestor_skip(uint16_t ret_s) { (void)ret_s; return -1; }
int  cpu_take_tailcall_return_context(uint16_t *e, uint8_t *h) { (void)e; (void)h; return 0; }
void cpu_dbg_funcname(const char *n) { (void)n; }
/* cpu_trace_* are all static-inline no-ops in cpu_trace.h for non-TRACE builds */
RecompReturn cpu_dispatch_pc_from(CpuState *c, uint32 pc, uint16 mr, uint32 src) {
    (void)c; (void)pc; (void)mr; (void)src; return RECOMP_RETURN_NORMAL;  /* leave S as-is */
}
RecompReturn cpu_dispatch_pc(CpuState *c, uint32 pc, uint16 mr) {
    (void)c; (void)pc; (void)mr; return RECOMP_RETURN_NORMAL;
}
int interp816_opcode_hook(uint32_t addr) { (void)addr; return 0; }

/* ── deterministic RNG (no Date/rand: reproducible) ── */
static uint64_t s_rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) {
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17;
    return (uint32_t)(s_rng >> 32);
}

typedef struct { uint16_t a, x, y, s, d; uint8_t db, C, Z, V, N, D, I; } St;

static int d_fail = 0, d_checks = 0;

static void run_one(const OpTest *op, const St *st, Interp816 *ip) {
    /* restore both buses to the reference pattern, then drop the opcode bytes
     * (identical in both) at $00:8000 — interp816 fetches them; the recomp has
     * the operand baked but may read $8000 as data, so both must match there. */
    memcpy(&RAM[WRAM], REF, WSZ);
    memcpy(&RAM2[WRAM], REF, WSZ);
    if (op->far) { memcpy(&RAM[FAR], REFF, FSZ); memcpy(&RAM2[FAR], REFF, FSZ); }
    memcpy(&RAM[0x8000], op->code, (size_t)op->len);   /* $00:8000 (not WRAM-mirrored) */
    memcpy(&RAM2[0x8000], op->code, (size_t)op->len);

    /* index-addressed ops: bound X/Y so base+index stays in WRAM (<$2000). */
    uint16_t xv = op->idx ? (uint16_t)(st->x % 0xF00) : st->x;
    uint16_t yv = op->idx ? (uint16_t)(st->y % 0xF00) : st->y;

    /* indirect ops: plant a pointer (into low WRAM) at the dp slot, identically
     * in both buses, so the indirect EA resolves into the filled WRAM window.
     * (dp,X) reads the pointer at D+dp+X; the rest at D+dp. */
    if (op->ind) {
        uint32_t dp = op->code[1];
        uint32_t loc = (op->ind == 3) ? (uint32_t)((st->d + dp + xv) & 0xFFFF)
                                      : (uint32_t)((st->d + dp) & 0xFFFF);
        uint32_t pr = rnd() % 0x1000;
        uint32_t c = mapa(0x00, loc);
        RAM[c]     = RAM2[c]     = (uint8_t)pr;
        RAM[c + 1] = RAM2[c + 1] = (uint8_t)(pr >> 8);
        if (op->ind == 2) { RAM[c + 2] = RAM2[c + 2] = 0x00; }  /* 24-bit: bank 0 */
    }

    /* recomp side */
    memset(&g_cpu, 0, sizeof g_cpu);
    g_cpu.ram = RAM; g_cpu.A = st->a; g_cpu.X = xv; g_cpu.Y = yv;
    g_cpu.S = st->s; g_cpu.D = st->d; g_cpu.DB = st->db; g_cpu.PB = 0;
    g_cpu.m_flag = op->m; g_cpu.x_flag = op->x; g_cpu.emulation = 0;
    g_cpu._flag_C = st->C; g_cpu._flag_Z = st->Z; g_cpu._flag_V = st->V;
    g_cpu._flag_N = st->N; g_cpu._flag_D = st->D; g_cpu._flag_I = st->I;
    g_cpu.host_return_valid = 0;
    cpu_mirrors_to_p(&g_cpu);
    op->fn(&g_cpu);
    g_cpu.S = (uint16)(g_cpu.S - 2);  /* undo the RTS frame pop */

    /* interp816 side: same state, one opcode */
    ip->a = st->a; ip->x = xv; ip->y = yv; ip->sp = st->s; ip->dp = st->d;
    ip->k = 0; ip->db = st->db; ip->pc = 0x8000; ip->e = false;
    ip->c = st->C; ip->z = st->Z; ip->v = st->V; ip->n = st->N;
    ip->d = st->D; ip->i = st->I; ip->mf = op->m; ip->xf = op->x;
    interp816_runOpcode(ip);

    /* diff registers + flags */
    d_checks++;
    int bad = 0;
    char msg[256] = {0};
#define CK(field, rv, iv) do { if ((rv) != (iv)) { bad = 1; \
    snprintf(msg + strlen(msg), sizeof msg - strlen(msg), " " field "=%X/%X", (unsigned)(rv), (unsigned)(iv)); } } while (0)
    CK("A", g_cpu.A, ip->a); CK("X", g_cpu.X, ip->x); CK("Y", g_cpu.Y, ip->y);
    CK("S", g_cpu.S, ip->sp); CK("D", g_cpu.D, ip->dp); CK("DB", g_cpu.DB, ip->db);
    CK("C", g_cpu._flag_C, ip->c); CK("Z", g_cpu._flag_Z, ip->z);
    CK("V", g_cpu._flag_V, ip->v); CK("N", g_cpu._flag_N, ip->n);
    CK("Df", g_cpu._flag_D, ip->d); CK("M", g_cpu.m_flag, ip->mf);
    CK("Xf", g_cpu.x_flag, ip->xf);
    /* diff the memory window for store/RMW ops (skip $8000-2: the opcode bytes) */
    if (op->wmem) {
        uint32_t base = op->far ? FAR : WRAM, sz = op->far ? FSZ : WSZ;
        for (uint32_t a = 0; a < sz; a++) {
            if (RAM[base + a] != RAM2[base + a]) { bad = 1;
                snprintf(msg + strlen(msg), sizeof msg - strlen(msg),
                         " MEM[%06X]=%02X/%02X", base + a, RAM[base + a], RAM2[base + a]); break; }
        }
    }
    if (bad) {
        d_fail++;
        if (d_fail <= 40)
            printf("  DIVERGE %-16s in:A=%04X X=%04X Y=%04X D=%04X DB=%X C%d Z%d V%d N%d D%d | r/i:%s\n",
                   op->name, st->a, xv, yv, st->d, st->db,
                   st->C, st->Z, st->V, st->N, st->D, msg);
    }
}

int main(void) {
    RAM = malloc(MEMSZ); RAM2 = malloc(MEMSZ); REF = malloc(WSZ); REFF = malloc(FSZ);
    memset(RAM, 0, MEMSZ); memset(RAM2, 0, MEMSZ);
    for (uint32_t i = 0; i < WSZ; i++) REF[i] = (uint8_t)rnd();   /* WRAM pattern */
    for (uint32_t i = 0; i < FSZ; i++) REFF[i] = (uint8_t)rnd();  /* far pattern  */
    Interp816 *ip = interp816_init(RAM2, i816_read, i816_write);  /* interp bus = RAM2 */

    const int ITERS = 3000;
    int per_op_fail[4096];
    memset(per_op_fail, 0, sizeof per_op_fail);

    printf("=== 65816 codegen vs interp816 differential ===\n");
    printf("opcodes: %d  iters/op: %d\n\n", g_nops, ITERS);
    for (int o = 0; o < g_nops; o++) {
        int f0 = d_fail;
        for (int it = 0; it < ITERS; it++) {
            St st;
            st.a = (uint16)rnd(); st.x = (uint16)rnd(); st.y = (uint16)rnd();
            /* D bounded so dp/dp,X EAs (D+dp+X, bounded index) stay in WRAM. */
            st.d = (uint16)(rnd() % 0xF00); st.db = (uint8)(rnd() & 1); st.s = 0x01FF;
            st.C = rnd() & 1; st.Z = rnd() & 1; st.V = rnd() & 1;
            st.N = rnd() & 1; st.D = rnd() & 1; st.I = rnd() & 1;
            run_one(&g_ops[o], &st, ip);
        }
        if (o < 4096) per_op_fail[o] = d_fail - f0;
    }

    /* per-opcode summary (only opcodes with divergences) */
    int diverging = 0;
    for (int o = 0; o < g_nops && o < 4096; o++)
        if (per_op_fail[o]) { diverging++;
            printf("  %-16s : %d/%d diverged\n", g_ops[o].name, per_op_fail[o], ITERS); }

    printf("\n==== %d checks, %d divergences across %d/%d opcodes ====\n",
           d_checks, d_fail, diverging, g_nops);
    printf(d_fail ? "RESULT: DIVERGENCES FOUND\n" : "RESULT: ALL OPCODES MATCH interp816\n");
    return d_fail ? 1 : 0;
}
