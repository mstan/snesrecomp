/*
 * cyc_trace -- cyc_watch ring demonstrator + self-test (Axis 2, step B).
 *
 * Steps the interp816 reference engine over a controlled 65816 program,
 * feeding an always-on cycle ring (cyc_ring) with BOTH the shared-authority
 * cycle count (snes_cycles.h, computed from pre-state + runtime predicates)
 * and interp816's native count. Then it QUERIES the ring: a window dump and
 * the two-anchor REGION delta of one loop iteration. The region cost is
 * asserted against the hand-computed datasheet value, so this doubles as a
 * regression test for the ring + the authority's runtime-predicate path.
 *
 * SCOPE: this uses a FLAT-RAM bus, not a full SNES bus (PPU/DMA/APU/MMIO).
 * It validates the ring + REGION mechanism and the authority's predicate
 * handling on a known code path. Booting a real ROM to an anchor needs a
 * SNES bus around interp816 (a separate component); for real-ROM cycle
 * ground truth the bsnes source hook is the intended oracle.
 *
 * Build (PowerShell / mingw):
 *   gcc -std=c99 -Wall -Wextra -I runner/src/snes -I tools/cyc_watch \
 *       tools/cyc_watch/cyc_trace.c tools/cyc_watch/cyc_ring.c \
 *       runner/src/snes/interp816.c -o tools/cyc_watch/cyc_trace.exe
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "interp816.h"
#include "snes_cycles.h"
#include "cyc_ring.h"

#define MEMSZ 0x1000000u
static uint8_t *MEM;
static uint8_t bus_read(void *m, uint32_t a)             { (void)m; return MEM[a & 0xFFFFFF]; }
static void    bus_write(void *m, uint32_t a, uint8_t v) { (void)m; MEM[a & 0xFFFFFF] = v; }
int interp816_opcode_hook(uint32_t addr) { (void)addr; return 0; }

static uint8_t rd(uint32_t a) { return MEM[a & 0xFFFFFF]; }
static uint16_t rd16(uint32_t a) { return rd(a) | (rd(a + 1) << 8); }

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { \
    g_fail++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* Compute the shared-authority CPU cycles for the opcode about to run / just
 * run, using interp816 pre-state and the runtime predicates. Must be called
 * with the pre-run register state captured (m,x,e,dp,X,Y) and the post-run pc
 * for branch-taken detection. */
static int authority_cycles(uint8_t op, uint32_t pc24,
                            int mf, int xf, int e, uint16_t dp,
                            uint16_t X, uint16_t Y,
                            uint16_t pc_before, uint16_t pc_after) {
    int dp_nz = (dp & 0xFF) != 0;

    int xcross = 0;
    if (SNES_XCROSS_ADD[op]) {                 /* read indexed op */
        uint8_t mode = SNES_OP_MODE[op];
        if (mode == SNES_MODE_ABS_X) {
            uint16_t o = rd16(pc24 + 1);
            xcross = (o >> 8) != (uint16_t)((o + X) >> 8);
        } else if (mode == SNES_MODE_ABS_Y) {
            uint16_t o = rd16(pc24 + 1);
            xcross = (o >> 8) != (uint16_t)((o + Y) >> 8);
        } else if (mode == SNES_MODE_INDIR_Y) {
            uint8_t  d = rd(pc24 + 1);
            uint16_t p = rd16((dp + d) & 0xFFFF);
            xcross = (p >> 8) != (uint16_t)((p + Y) >> 8);
        }
    }

    int btaken = 0, bcross = 0;
    if (SNES_BRANCH_CLASS[op]) {
        int len = (SNES_OP_MODE[op] == SNES_MODE_REL16) ? 3 : 2;
        uint16_t fall = (uint16_t)(pc_before + len);
        btaken = (pc_after != fall);
        if (btaken)
            bcross = ((pc_before + len) & 0xFF00) != (pc_after & 0xFF00);
    }
    return snes_instr_cpu_cycles(op, mf, xf, e, dp_nz, xcross, btaken, bcross);
}

int main(void) {
    MEM = malloc(MEMSZ);
    memset(MEM, 0, MEMSZ);
    Interp816 *cpu = interp816_init(NULL, bus_read, bus_write);

    /* Program at $00:8000 — enter native 16-bit, then a 3-iteration loop that
     * read-modify-writes $1000, ending in STP. */
    const uint8_t prog[] = {
        0x18,             /* 8000 CLC                 */
        0xFB,             /* 8001 XCE  -> native      */
        0xC2, 0x30,       /* 8002 REP #$30 -> 16-bit  */
        0xA2, 0x03, 0x00, /* 8004 LDX #$0003          */
        /* START = $8007 */
        0xAD, 0x00, 0x10, /* 8007 LDA $1000           */
        0x1A,             /* 800A INC A               */
        0x8D, 0x00, 0x10, /* 800B STA $1000           */
        0xCA,             /* 800E DEX                 */
        0xD0, 0xF6,       /* 800F BNE $8007           */
        /* END = $8011 */
        0xDB,             /* 8011 STP                 */
    };
    const uint32_t START_PC = 0x008007, END_PC = 0x008011;
    memcpy(&MEM[0x8000], prog, sizeof prog);

    interp816_reset(cpu);
    cpu->pc = 0x8000; cpu->k = 0;

    CycRing ring;
    cyc_ring_init(&ring, 1u << 12);

    printf("== stepping interp816, filling the always-on cycle ring ==\n");
    for (int budget = 0; budget < 1000 && !cpu->stopped; budget++) {
        uint32_t pc24 = ((uint32_t)cpu->k << 16) | cpu->pc;
        uint8_t  op = rd(pc24);
        int mf = cpu->mf, xf = cpu->xf, e = cpu->e;
        uint16_t dp = cpu->dp, X = cpu->x, Y = cpu->y;
        uint16_t pc_before = cpu->pc;

        int ref = interp816_runOpcode(cpu);
        uint16_t pc_after = cpu->pc;

        int auth = authority_cycles(op, pc24, mf, xf, e, dp, X, Y,
                                    pc_before, pc_after);
        uint32_t master = (uint32_t)auth * (uint32_t)snes_region_speed(pc24, 0);
        cyc_ring_push(&ring, pc24, op, (uint16_t)auth, (uint16_t)ref, master);
    }
    printf("   executed %llu instructions\n", (unsigned long long)ring.total);

    printf("\n== window dump: first 9 records (seq pc op auth ref master) ==\n");
    for (uint64_t s = 0; s < 9 && s < ring.total; s++) {
        const CycRec *r = cyc_ring_get(&ring, s);
        printf("   #%llu  $%06X  op $%02X  auth=%u ref=%u master=%u\n",
               (unsigned long long)r->seq, r->pc24, r->opcode,
               r->cyc_auth, r->cyc_ref, r->master);
    }

    printf("\n== two-anchor REGION: one loop iteration ($8007 -> next $8007) ==\n");
    /* start_pc == end_pc => cost between consecutive crossings = one iter. */
    CycRegion it1 = cyc_ring_region_anchors(&ring, START_PC, START_PC, 0);
    printf("   iter1: %llu insns, auth=%llu ref=%llu master=%llu\n",
           (unsigned long long)it1.count, (unsigned long long)it1.sum_auth,
           (unsigned long long)it1.sum_ref, (unsigned long long)it1.sum_master);
    /* Hand count (native 16-bit): LDA abs 5 + INC A 2 + STA abs 5 + DEX 2 +
       BNE taken 3 = 17 CPU cycles; 5 instructions. */
    CHECK(it1.count == 5, "iter1 insn count %llu exp 5", (unsigned long long)it1.count);
    CHECK(it1.sum_auth == 17, "iter1 authority cyc %llu exp 17", (unsigned long long)it1.sum_auth);
    CHECK(it1.sum_ref == 17, "iter1 reference cyc %llu exp 17", (unsigned long long)it1.sum_ref);

    /* Region stability: the second iteration must cost the same. */
    uint64_t s0 = cyc_ring_find_pc(&ring, START_PC, 0);
    CycRegion it2 = cyc_ring_region_anchors(&ring, START_PC, START_PC, s0 + 1);
    printf("   iter2: %llu insns, auth=%llu ref=%llu\n",
           (unsigned long long)it2.count, (unsigned long long)it2.sum_auth,
           (unsigned long long)it2.sum_ref);
    CHECK(it2.sum_auth == 17, "iter2 authority cyc %llu exp 17 (region unstable)",
          (unsigned long long)it2.sum_auth);

    printf("\n== two-anchor REGION: full loop ($8007 entry -> $8011 exit) ==\n");
    CycRegion loop = cyc_ring_region_anchors(&ring, START_PC, END_PC, 0);
    printf("   loop:  %llu insns, auth=%llu ref=%llu\n",
           (unsigned long long)loop.count, (unsigned long long)loop.sum_auth,
           (unsigned long long)loop.sum_ref);
    /* 3 iterations: two with BNE taken (17 each) + final with BNE not-taken
       (LDA 5 + INC 2 + STA 5 + DEX 2 + BNE 2 = 16) = 50 cyc over 15 insns. */
    CHECK(loop.count == 15, "loop insn count %llu exp 15", (unsigned long long)loop.count);
    CHECK(loop.sum_auth == 50, "loop authority cyc %llu exp 50", (unsigned long long)loop.sum_auth);

    printf("\n== whole-trace model agreement (authority vs reference) ==\n");
    CycRegion all = cyc_ring_region(&ring, 0, ring.total);
    printf("   total auth=%llu ref=%llu master=%llu over %llu insns\n",
           (unsigned long long)all.sum_auth, (unsigned long long)all.sum_ref,
           (unsigned long long)all.sum_master, (unsigned long long)all.count);
    /* This program touches none of the 4 documented divergence sites, so the
       authority and the reference must agree exactly over the whole trace. */
    CHECK(all.sum_auth == all.sum_ref,
          "whole-trace auth %llu != ref %llu", (unsigned long long)all.sum_auth,
          (unsigned long long)all.sum_ref);

    cyc_ring_free(&ring);
    free(MEM);
    printf("\n==== cyc_trace: %s ====\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
