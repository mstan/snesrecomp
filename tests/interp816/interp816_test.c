/*
 * interp816 Phase-0 validation harness.
 *
 * Proves the vendored + renamed + de-cruft'd LakeSnes core is semantically
 * intact: directed 65816 opcode sequences over a flat-RAM bus, asserting
 * register / flag / memory results. Build/run via WSL (validation only).
 *   gcc -I runner/src/snes _interp_recover/interp816_test.c \
 *       runner/src/snes/interp816.c -o _interp_recover/interp816_test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "interp816.h"

#define MEMSZ 0x1000000u            /* 16 MB, 24-bit address space */
static uint8_t *MEM;

static uint8_t bus_read(void *mem, uint32_t adr)            { (void)mem; return MEM[adr & 0xFFFFFF]; }
static void    bus_write(void *mem, uint32_t adr, uint8_t v){ (void)mem; MEM[adr & 0xFFFFFF] = v; }

/* BRK bridge seam stub — Phase 0 has no bridge; treat BRK as a no-op. */
int interp816_opcode_hook(uint32_t addr) { (void)addr; return 0; }

static int g_fail = 0, g_check = 0;
#define CHECK(cond, ...) do { g_check++; if (!(cond)) { \
    g_fail++; printf("    FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static Interp816 *g_cpu;

/* Reset, lay code at 00:8000, point PC at it. Returns the cpu. */
static Interp816 *prep(const uint8_t *code, int len) {
  memset(MEM, 0, MEMSZ);
  interp816_reset(g_cpu);            /* e=1, mf=xf=1, sp=0x100, DB=0 */
  memcpy(&MEM[0x8000], code, (size_t)len);
  g_cpu->pc = 0x8000;
  g_cpu->k  = 0;
  return g_cpu;
}
static void run(Interp816 *cpu, int n) { for (int i = 0; i < n; i++) interp816_runOpcode(cpu); }

int main(void) {
  MEM = malloc(MEMSZ);
  g_cpu = interp816_init(NULL, bus_read, bus_write);

  /* T1: LDA #$42 (8-bit) */
  { uint8_t c[] = {0xA9,0x42};            Interp816 *p = prep(c,sizeof c); run(p,1);
    printf("T1 LDA #$42\n");
    CHECK((p->a & 0xFF)==0x42, "A.lo=%02X exp 42", p->a & 0xFF);
    CHECK(!p->z && !p->n, "z=%d n=%d exp 0/0", p->z, p->n); }

  /* T2: LDA #$00 -> Z set */
  { uint8_t c[] = {0xA9,0x00};            Interp816 *p = prep(c,sizeof c); run(p,1);
    printf("T2 LDA #$00\n");
    CHECK(p->z, "z=%d exp 1", p->z); }

  /* T3: LDA #$80 -> N set */
  { uint8_t c[] = {0xA9,0x80};            Interp816 *p = prep(c,sizeof c); run(p,1);
    printf("T3 LDA #$80\n");
    CHECK(p->n, "n=%d exp 1", p->n); }

  /* T4: CLC; XCE; REP #$30; LDA #$1234  -> native 16-bit, A=0x1234 */
  { uint8_t c[] = {0x18,0xFB,0xC2,0x30,0xA9,0x34,0x12}; Interp816 *p = prep(c,sizeof c); run(p,4);
    printf("T4 enter native 16-bit + LDA #$1234\n");
    CHECK(!p->e, "e=%d exp 0 (native)", p->e);
    CHECK(!p->mf && !p->xf, "mf=%d xf=%d exp 0/0 (16-bit)", p->mf, p->xf);
    CHECK(p->a==0x1234, "A=%04X exp 1234", p->a); }

  /* T5: 16-bit store then reload from RAM */
  { uint8_t c[] = {0x18,0xFB,0xC2,0x30, 0xA9,0x34,0x12, 0x8D,0x10,0x00,
                   0xA9,0x00,0x00, 0xAD,0x10,0x00};
    Interp816 *p = prep(c,sizeof c); run(p,7);
    printf("T5 STA $0010 / LDA $0010 (16-bit)\n");
    CHECK(MEM[0x10]==0x34 && MEM[0x11]==0x12, "mem[10,11]=%02X %02X exp 34 12", MEM[0x10], MEM[0x11]);
    CHECK(p->a==0x1234, "A=%04X exp 1234", p->a); }

  /* T6: LDX #$FF; INX -> 8-bit wrap to 0, Z set */
  { uint8_t c[] = {0xA2,0xFF,0xE8};       Interp816 *p = prep(c,sizeof c); run(p,2);
    printf("T6 LDX #$FF; INX (8-bit wrap)\n");
    CHECK((p->x & 0xFF)==0x00, "X.lo=%02X exp 00", p->x & 0xFF);
    CHECK(p->z, "z=%d exp 1", p->z); }

  /* T7: CLC; LDA #$10; ADC #$22 -> 0x32 (binary) */
  { uint8_t c[] = {0x18,0xA9,0x10,0x69,0x22}; Interp816 *p = prep(c,sizeof c); run(p,3);
    printf("T7 ADC binary 10+22\n");
    CHECK((p->a & 0xFF)==0x32, "A.lo=%02X exp 32", p->a & 0xFF); }

  /* T8: CLC; SED; LDA #$19; ADC #$01 -> 0x20 (decimal) */
  { uint8_t c[] = {0x18,0xF8,0xA9,0x19,0x69,0x01}; Interp816 *p = prep(c,sizeof c); run(p,4);
    printf("T8 ADC decimal 19+01\n");
    CHECK((p->a & 0xFF)==0x20, "A.lo=%02X exp 20 (BCD)", p->a & 0xFF); }

  /* T9: LDA #$55; PHA; LDA #$00; PLA -> A=0x55 */
  { uint8_t c[] = {0xA9,0x55,0x48,0xA9,0x00,0x68}; Interp816 *p = prep(c,sizeof c); run(p,4);
    printf("T9 PHA / PLA\n");
    CHECK((p->a & 0xFF)==0x55, "A.lo=%02X exp 55", p->a & 0xFF); }

  /* T10: LDA #$77; TAX -> X.lo=0x77 */
  { uint8_t c[] = {0xA9,0x77,0xAA};       Interp816 *p = prep(c,sizeof c); run(p,2);
    printf("T10 TAX\n");
    CHECK((p->x & 0xFF)==0x77, "X.lo=%02X exp 77", p->x & 0xFF); }

  /* T11: LDA #$AB; XBA -> A=0xAB00 (swap high/low of 16-bit A) */
  { uint8_t c[] = {0xA9,0xAB,0xEB};       Interp816 *p = prep(c,sizeof c); run(p,2);
    printf("T11 XBA\n");
    CHECK(p->a==0xAB00, "A=%04X exp AB00", p->a); }

  /* T12: LDA #$01; BNE +2 (skip LDA #$FF); BRK -> A stays 0x01 (branch taken) */
  { uint8_t c[] = {0xA9,0x01, 0xD0,0x02, 0xA9,0xFF, 0x00}; Interp816 *p = prep(c,sizeof c); run(p,3);
    printf("T12 BNE taken\n");
    CHECK((p->a & 0xFF)==0x01, "A.lo=%02X exp 01 (branch not taken?)", p->a & 0xFF); }

  printf("\n==== interp816 Phase-0: %d/%d checks passed ====\n", g_check - g_fail, g_check);
  if (g_fail) { printf("RESULT: FAIL (%d)\n", g_fail); return 1; }
  printf("RESULT: PASS\n");
  return 0;
}
