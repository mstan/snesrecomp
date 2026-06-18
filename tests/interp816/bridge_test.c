/*
 * interp_bridge Phase-1 contract harness (no game / no ROM).
 *
 * Proves the interp<->AOT bridge mechanics deterministically with fakes:
 *   - cpu_read8/cpu_write8  -> a flat RAM (the bus the bridge routes through);
 *   - cpu_dispatch_pc / cpu_dispatch_has_entry -> ONE known "compiled" entry
 *     whose fake body mutates A and pops its return frame (modelling a real
 *     AOT function's RTS: pop frame, dispatch-miss on return addr, S restored).
 *
 * Scenarios:
 *   S1: interp routine that JSRs into the compiled entry -> the bounce runs the
 *       compiled body, state syncs, stack stays balanced, resume at return addr.
 *   S2: pure interp routine (no call) -> exits balanced, no bounce.
 *   S3: interp routine that JSRs a NON-compiled target -> interpreted through,
 *       its RTS returns to caller level (no premature exit), final RTS exits.
 *
 * Build/run: tests/interp816/run.sh (WSL gcc). Validation only.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "interp_bridge.h"   /* -> cpu_state.h (types, inline frame helpers) */

#define MEMSZ 0x1000000u
static uint8_t *RAM;
static int      g_aot_called;
#define FAKE_AOT 0x008100u

/* ── fakes the bridge links against (cpu_state.c provides these in prod) ── */
uint8 cpu_read8(CpuState *cpu, uint8 bank, uint16 addr) {
    (void)cpu; return RAM[(((uint32)bank << 16) | addr) & 0xFFFFFF];
}
void cpu_write8(CpuState *cpu, uint8 bank, uint16 addr, uint8 v) {
    (void)cpu; RAM[(((uint32)bank << 16) | addr) & 0xFFFFFF] = v;
}
int cpu_dispatch_has_entry(CpuState *cpu, uint32 pc24) {
    (void)cpu; return (pc24 & 0xFFFFFF) == FAKE_AOT;
}
static int g_abandon_called;
RecompReturn cpu_unresolved_abandon_balanced(CpuState *cpu, uint32 site_pc24,
                                             uint16 entry_s, uint8 hrv) {
    (void)site_pc24; g_abandon_called++;
    cpu->S = (uint16)(entry_s + hrv);
    return RECOMP_RETURN_NORMAL;
}
RecompReturn cpu_dispatch_pc(CpuState *cpu, uint32 pc24, uint16 miss_restore) {
    if ((pc24 & 0xFFFFFF) == FAKE_AOT) {
        g_aot_called++;
        cpu->A = (uint16)(cpu->A + 0x0100);     /* observable "compiled" work */
        cpu->S = (uint16)(cpu->S + 2);          /* models RTS popping its frame */
        return RECOMP_RETURN_NORMAL;
    }
    cpu->S = miss_restore;
    return RECOMP_RETURN_NORMAL;
}

static int g_fail = 0, g_check = 0;
#define CHECK(cond, ...) do { g_check++; if (!(cond)) { \
    g_fail++; printf("    FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static CpuState g_c;
static void init_cpu(void) {
    memset(&g_c, 0, sizeof g_c);
    g_c.S = 0x01FF; g_c.emulation = 1; g_c.m_flag = 1; g_c.x_flag = 1;
    g_c._flag_I = 1; g_c.ram = RAM; cpu_mirrors_to_p(&g_c);
}
static void load(uint32 pc24, const uint8_t *code, int len) {
    memcpy(&RAM[pc24 & 0xFFFFFF], code, (size_t)len);
}

int main(void) {
    RAM = malloc(MEMSZ);

    /* S1: LDA #$01 ; JSR $8100 (compiled) ; RTS */
    { memset(RAM, 0, MEMSZ); init_cpu(); g_aot_called = 0;
      uint8_t c[] = {0xA9,0x01, 0x20,0x00,0x81, 0x60};
      load(0x8000, c, sizeof c);
      cpu_push_jsr_return_frame(&g_c);          /* sentinel return frame */
      int rc = interp_bridge_run(&g_c, 0x008000);
      printf("S1 JSR-into-compiled bounce\n");
      CHECK(rc == 1, "rc=%d exp 1", rc);
      CHECK(g_aot_called == 1, "aot_called=%d exp 1", g_aot_called);
      CHECK(g_c.A == 0x0101, "A=%04X exp 0101 (01 from LDA + 0100 from AOT)", g_c.A);
      CHECK(g_c.S == 0x01FF, "S=%04X exp 01FF (balanced)", g_c.S); }

    /* S2: LDA #$09 ; RTS  (no call) */
    { memset(RAM, 0, MEMSZ); init_cpu(); g_aot_called = 0;
      uint8_t c[] = {0xA9,0x09, 0x60};
      load(0x8000, c, sizeof c);
      cpu_push_jsr_return_frame(&g_c);
      int rc = interp_bridge_run(&g_c, 0x008000);
      printf("S2 pure interp routine\n");
      CHECK(rc == 1, "rc=%d exp 1", rc);
      CHECK(g_aot_called == 0, "aot_called=%d exp 0", g_aot_called);
      CHECK((g_c.A & 0xFF) == 0x09, "A.lo=%02X exp 09", g_c.A & 0xFF);
      CHECK(g_c.S == 0x01FF, "S=%04X exp 01FF", g_c.S); }

    /* S3: JSR $8200 (NOT compiled) ; RTS  /  $8200: LDA #$33 ; RTS */
    { memset(RAM, 0, MEMSZ); init_cpu(); g_aot_called = 0;
      uint8_t caller[] = {0x20,0x00,0x82, 0x60};
      uint8_t callee[] = {0xA9,0x33, 0x60};
      load(0x8000, caller, sizeof caller);
      load(0x8200, callee, sizeof callee);
      cpu_push_jsr_return_frame(&g_c);
      int rc = interp_bridge_run(&g_c, 0x008000);
      printf("S3 interpret-through non-compiled call\n");
      CHECK(rc == 1, "rc=%d exp 1", rc);
      CHECK(g_aot_called == 0, "aot_called=%d exp 0 (no compiled body)", g_aot_called);
      CHECK((g_c.A & 0xFF) == 0x33, "A.lo=%02X exp 33", g_c.A & 0xFF);
      CHECK(g_c.S == 0x01FF, "S=%04X exp 01FF (balanced through nested RTS)", g_c.S); }

    /* S4: interp_tier_dispatch (the production tier-down entry, tail-dispatch
     * shape): a caller frame is on the stack (as after a JSR into the
     * dispatcher); the dispatched target runs and RTSes past entry. */
    { memset(RAM, 0, MEMSZ); init_cpu(); g_aot_called = 0;
      uint8_t c[] = {0xA9,0x07, 0x60};      /* $8000: LDA #$07 ; RTS */
      load(0x8000, c, sizeof c);
      long hits0 = interp_tier_hit_count();
      cpu_push_jsr_return_frame(&g_c);       /* the (inherited) caller frame */
      RecompReturn r = interp_tier_dispatch(&g_c, 0x008000);
      printf("S4 interp_tier_dispatch (tail-dispatch entry)\n");
      CHECK(r == RECOMP_RETURN_NORMAL, "r=%d exp NORMAL(0)", (int)r);
      CHECK((g_c.A & 0xFF) == 0x07, "A.lo=%02X exp 07", g_c.A & 0xFF);
      CHECK(g_c.S == 0x01FF, "S=%04X exp 01FF (caller frame consumed)", g_c.S);
      CHECK(interp_tier_hit_count() == hits0 + 1, "hit_count delta exp 1"); }

    /* S5: interp_tier_dispatch_balanced (SM abandon-site upgrade). A clean
     * routine interprets to completion -> NORMAL, balanced, abandon NOT used. */
    { memset(RAM, 0, MEMSZ); init_cpu(); g_aot_called = 0; g_abandon_called = 0;
      uint8_t c[] = {0xA9,0x0C, 0x60};      /* $8000: LDA #$0C ; RTS */
      load(0x8000, c, sizeof c);
      cpu_push_jsr_return_frame(&g_c);       /* inherited caller frame (hrv=2) */
      uint16 entry_s = g_c.S;                /* function entry S = after caller's push */
      RecompReturn r = interp_tier_dispatch_balanced(&g_c, 0x008000, 0x00C0DE,
                                                     entry_s, 2);
      printf("S5 interp_tier_dispatch_balanced (clean -> interpret, no abandon)\n");
      CHECK(r == RECOMP_RETURN_NORMAL, "r=%d exp NORMAL", (int)r);
      CHECK((g_c.A & 0xFF) == 0x0C, "A.lo=%02X exp 0C (interpreted)", g_c.A & 0xFF);
      CHECK(g_abandon_called == 0, "abandon_called=%d exp 0 (clean interp)", g_abandon_called);
      CHECK(g_c.S == (uint16)(entry_s + 2), "S=%04X exp %04X (frame popped)", g_c.S, (uint16)(entry_s + 2)); }

    printf("\n==== interp_bridge Phase-1: %d/%d checks passed ====\n", g_check - g_fail, g_check);
    if (g_fail) { printf("RESULT: FAIL (%d)\n", g_fail); return 1; }
    printf("RESULT: PASS\n");
    return 0;
}
