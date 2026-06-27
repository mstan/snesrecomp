/*
 * cyc_equiv -- Axis-2 cycle-equivalence harness (step B, first validation).
 *
 * Cross-checks the shared cost model (recompiler/snes_cycles.py, baked into
 * runner/src/snes/snes_cycles.h) against an INDEPENDENT implementation: the
 * vendored LakeSnes interpreter (interp816), which carries its own
 * cyclesPerOpcode[] table + inline modifier logic. This is "reference shelf,
 * not self-agreement": two independently-sourced cycle models, executed on
 * directed 65816 sequences, must agree per opcode.
 *
 * The 256-entry BASE table already matches LakeSnes byte-for-byte (static
 * check in tests/test_snes_cycles.py via the table parse). This harness
 * validates the MODIFIER application in execution (m=0 / x=0 / D.l!=0 /
 * native RTI-BRK-COP / branch taken) and DELIBERATELY exercises the indexed
 * page-cross cases to surface where the two models legitimately differ.
 *
 * Build (Windows, mingw):
 *   gcc -std=c99 -Wall -Wextra -I runner/src/snes \
 *       tools/cyc_watch/cyc_equiv.c runner/src/snes/interp816.c \
 *       -o tools/cyc_watch/cyc_equiv.exe
 * Exit code 0 = all asserted cases agree (measured-divergence cases are
 * reported but do not fail the run; they document a known LakeSnes deviation).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "interp816.h"
#include "snes_cycles.h"

#define MEMSZ 0x1000000u
static uint8_t *MEM;
static uint8_t bus_read(void *m, uint32_t a)             { (void)m; return MEM[a & 0xFFFFFF]; }
static void    bus_write(void *m, uint32_t a, uint8_t v) { (void)m; MEM[a & 0xFFFFFF] = v; }
/* BRK bridge seam stub (standalone). */
int interp816_opcode_hook(uint32_t addr) { (void)addr; return 0; }

static Interp816 *g_cpu;
static int g_fail = 0, g_check = 0, g_diverge = 0;

/* Pre-state for one directed opcode run. */
typedef struct {
    int e, mf, xf;          /* emulation / mem-width / index-width bits */
    uint16_t dp, x, y;      /* D register, index regs */
    uint8_t db;             /* data bank */
} State;

/* Lay `code` at $00:8000, apply pre-state, run ONE opcode, return cycles. */
static int run_one(const uint8_t *code, int len, State s) {
    memset(MEM, 0, MEMSZ);
    interp816_reset(g_cpu);
    memcpy(&MEM[0x8000], code, (size_t)len);
    g_cpu->pc = 0x8000; g_cpu->k = 0;
    g_cpu->e = s.e; g_cpu->mf = s.mf; g_cpu->xf = s.xf;
    g_cpu->dp = s.dp; g_cpu->x = s.x; g_cpu->y = s.y; g_cpu->db = s.db;
    return interp816_runOpcode(g_cpu);
}

/* Assert interp816 cycles == authority cycles for opcode `op`. */
static void expect_eq(const char *name, const uint8_t *code, int len, State s,
                      uint8_t op, int dp_nz, int xcross, int btaken, int bcross) {
    int got = run_one(code, len, s);
    int want = snes_instr_cpu_cycles(op, s.mf, s.xf, s.e,
                                     dp_nz, xcross, btaken, bcross);
    g_check++;
    if (got != want) {
        g_fail++;
        printf("  FAIL %-28s op $%02X: interp816=%d authority=%d\n",
               name, op, got, want);
    } else {
        printf("  ok   %-28s op $%02X: %d cyc\n", name, op, got);
    }
}

/* Report (without failing) where the two models legitimately diverge. */
static void measure(const char *name, const uint8_t *code, int len, State s,
                    uint8_t op, int dp_nz, int xcross, int btaken, int bcross) {
    int got = run_one(code, len, s);
    int want = snes_instr_cpu_cycles(op, s.mf, s.xf, s.e,
                                     dp_nz, xcross, btaken, bcross);
    g_check++;
    if (got != want) {
        g_diverge++;
        printf("  DIVERGE %-25s op $%02X: interp816=%d authority=%d  (%s)\n",
               name, op, got, want,
               got > want ? "LakeSnes over-counts" : "LakeSnes under-counts");
    } else {
        printf("  ok(=)   %-25s op $%02X: %d cyc\n", name, op, got);
    }
}

int main(void) {
    MEM = malloc(MEMSZ);
    g_cpu = interp816_init(NULL, bus_read, bus_write);
    State emu  = {1,1,1, 0,0,0,0};   /* reset: emulation, 8-bit */
    State nat8 = {0,1,1, 0,0,0,0};   /* native, 8-bit A + index */
    State nat16= {0,0,0, 0,0,0,0};   /* native, 16-bit A + index */

    printf("== base + width (m=0 / x=0) ==\n");
    /* LDA #imm: 2 (+1 m=0) */
    { uint8_t c[]={0xA9,0,0}; expect_eq("LDA #imm 8b",  c,3, nat8,  0xA9,0,0,0,0);
                              expect_eq("LDA #imm 16b", c,3, nat16, 0xA9,0,0,0,0); }
    /* LDA abs: 4 (+1 m=0) */
    { uint8_t c[]={0xAD,0,0}; expect_eq("LDA abs 8b",   c,3, nat8,  0xAD,0,0,0,0);
                              expect_eq("LDA abs 16b",  c,3, nat16, 0xAD,0,0,0,0); }
    /* ASL abs (RMW): 6 (+2 m=0) */
    { uint8_t c[]={0x0E,0,0}; expect_eq("ASL abs 8b",   c,3, nat8,  0x0E,0,0,0,0);
                              expect_eq("ASL abs 16b",  c,3, nat16, 0x0E,0,0,0,0); }
    /* LDX #imm: 2 (+1 x=0) */
    { uint8_t c[]={0xA2,0,0}; expect_eq("LDX #imm 8b",  c,3, nat8,  0xA2,0,0,0,0);
                              expect_eq("LDX #imm 16b", c,3, nat16, 0xA2,0,0,0,0); }
    /* PHA: 3 (+1 m=0); PLA: 4 (+1 m=0) */
    { uint8_t c[]={0x48};     expect_eq("PHA 8b",  c,1, nat8,  0x48,0,0,0,0);
                              expect_eq("PHA 16b", c,1, nat16, 0x48,0,0,0,0); }
    { uint8_t c[]={0x68};     expect_eq("PLA 16b", c,1, nat16, 0x68,0,0,0,0); }

    printf("== D.l != 0 (direct-page) ==\n");
    State dpz  = {0,1,1, 0x0000,0,0,0};   /* D.l == 0 */
    State dpnz = {0,1,1, 0x0080,0,0,0};   /* D.l == 0x80 != 0 */
    { uint8_t c[]={0xA5,0x10}; expect_eq("LDA dp  D.l=0",  c,2, dpz,  0xA5,0,0,0,0);
                               expect_eq("LDA dp  D.l!=0", c,2, dpnz, 0xA5,1,0,0,0); }

    printf("== native RTI / COP (datasheet: +1 when e=0; SNES runs native) ==\n");
    /* Datasheet: RTI = 6 (+1 if e=0), COP = 7 (+1 if e=0). LakeSnes applies
       the native extra cycle UNCONDITIONALLY (interp816.c rti/cop handlers:
       `cyclesUsed++` with no `if(!e)` guard) -> correct in native (the SNES
       game state), +1 too high in emulation. Native cases agree; emulation
       cases are documented divergences. BRK ($00) is excluded: this snesrecomp
       adaptation repurposes BRK as the AOT-bridge trap (interp816_opcode_hook),
       so its cycle count is not a model of the BRK interrupt. */
    { uint8_t c[]={0x40};   expect_eq("RTI native",  c,1, nat8, 0x40,0,0,0,0);
                            measure  ("RTI emu",     c,1, emu,  0x40,0,0,0,0); }
    { uint8_t c[]={0x02,0}; expect_eq("COP native",  c,2, nat8, 0x02,0,0,0,0);
                            measure  ("COP emu",      c,2, emu,  0x02,0,0,0,0); }

    printf("== branches (taken adds 1; emulation page-cross adds 1 more) ==\n");
    /* BRA $00 (to next insn): always taken, native => no page-cross add. */
    { uint8_t c[]={0x80,0x00}; expect_eq("BRA taken nat", c,2, nat8, 0x80,0,0,0,0); }
    /* BNE not taken: set Z so branch falls through. After reset Z=0 => BNE
       WOULD take; set Z=1 to make it not-taken. */
    { g_check=g_check; uint8_t c[]={0xD0,0x10}; memset(MEM,0,MEMSZ);
      interp816_reset(g_cpu); g_cpu->e=0; g_cpu->mf=g_cpu->xf=1;
      g_cpu->z=true; memcpy(&MEM[0x8000],c,2); g_cpu->pc=0x8000; g_cpu->k=0;
      int got=interp816_runOpcode(g_cpu);
      int want=snes_instr_cpu_cycles(0xD0,1,1,0,0,0,/*taken*/0,0);
      g_check++; if(got!=want){g_fail++;printf("  FAIL BNE not-taken op $D0: interp816=%d authority=%d\n",got,want);}
      else printf("  ok   BNE not-taken              op $D0: %d cyc\n", got); }
    /* BNE taken (Z=0 after reset): +1 */
    { uint8_t c[]={0xD0,0x10}; memset(MEM,0,MEMSZ);
      interp816_reset(g_cpu); g_cpu->e=0; g_cpu->mf=g_cpu->xf=1;
      memcpy(&MEM[0x8000],c,2); g_cpu->pc=0x8000; g_cpu->k=0;
      int got=interp816_runOpcode(g_cpu);
      int want=snes_instr_cpu_cycles(0xD0,1,1,0,0,0,/*taken*/1,0);
      g_check++; if(got!=want){g_fail++;printf("  FAIL BNE taken op $D0: interp816=%d authority=%d\n",got,want);}
      else printf("  ok   BNE taken nat             op $D0: %d cyc\n", got); }

    printf("== indexed page-cross: READ (datasheet +1; LakeSnes omits) ==\n");
    /* LDA $80FF,X with X=1 -> effective $8100 (crosses page). Authority: read
       cross => +1. */
    { uint8_t c[]={0xBD,0xFF,0x80}; State s={0,1,1,0,0x01,0,0};
      measure("LDA abs,X cross 8b", c,3, s, 0xBD, 0, /*xcross*/1, 0,0); }
    { uint8_t c[]={0xBD,0x00,0x80}; State s={0,1,1,0,0x01,0,0};
      expect_eq("LDA abs,X no-cross 8b", c,3, s, 0xBD, 0, 0, 0,0); }

    printf("== indexed page-cross: WRITE (datasheet fixed; LakeSnes adds) ==\n");
    /* STA $80FF,X with X=1 -> crosses. Authority: store base already fixed
       (no cross add). LakeSnes: adds +1 for write on cross. */
    { uint8_t c[]={0x9D,0xFF,0x80}; State s={0,1,1,0,0x01,0,0};
      measure("STA abs,X cross 8b", c,3, s, 0x9D, 0, 0, 0,0); }
    { uint8_t c[]={0x9D,0x00,0x80}; State s={0,1,1,0,0x01,0,0};
      expect_eq("STA abs,X no-cross 8b", c,3, s, 0x9D, 0, 0, 0,0); }

    printf("\n==== cyc_equiv: %d/%d asserted cases agree; %d measured divergences ====\n",
           g_check - g_fail - g_diverge, g_check, g_diverge);
    if (g_fail) { printf("RESULT: FAIL (%d)\n", g_fail); return 1; }
    printf("RESULT: PASS (asserted-equivalence cases)\n");
    return 0;
}
