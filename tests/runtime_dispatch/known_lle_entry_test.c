/* Contract test for dispatch-table row presence versus exact AOT presence.
 * Build cpu_state.c with function sections + linker GC so this harness needs
 * only the dependencies of cpu_dispatch_pc_from. */
#include <stdio.h>
#include <string.h>

#include "cpu_state.h"
#include "snes/cart.h"
#include "snes/snes.h"
#include "snes/superfx.h"

/* cpu_state.c's optional write-state recorder reads the interpreter PC. The
 * production definition lives in interp_bridge.c, which this focused dispatch
 * harness intentionally does not link. */
uint32_t g_interp_wlog_pc24 = 0;

static int g_aot_calls;
static int g_lle_calls;
static uint32 g_lle_target;
static uint32 g_lle_site;
static uint16 g_lle_restore_s;

int snes_frame_counter;
const char *g_last_recomp_func = "known_lle_entry_test";
uint8 *g_sram;
int g_sram_size;
uint64_t g_main_cpu_cycles_estimate;
uint64_t g_apu_pace_cycles_estimate;
Snes *g_snes;

static uint8 g_test_rom[0x400000];

uint8 ReadReg(uint16 reg) { (void)reg; return 0; }
uint16 ReadRegWord(uint16 reg) { (void)reg; return 0; }
void WriteReg(uint16 reg, uint8 value) { (void)reg; (void)value; }
void WriteRegWord(uint16 reg, uint16 value) { (void)reg; (void)value; }
uint8 *RomPtr(uint32 addr) {
    return cart_getRomPtr(g_snes->cart, (uint8)(addr >> 16), (uint16)addr);
}
SuperFx *superfx_create(uint8_t *rom, uint32_t rom_size, uint8_t *ram,
                        uint32_t ram_size) {
    (void)rom; (void)rom_size; (void)ram; (void)ram_size; return NULL;
}
void superfx_destroy(SuperFx *fx) { (void)fx; }
void superfx_reset(SuperFx *fx) { (void)fx; }
void superfx_sync(SuperFx *fx, uint64_t master_clock) {
    (void)fx; (void)master_clock;
}
uint8_t superfx_cpu_read_io(SuperFx *fx, uint16_t address) {
    (void)fx; (void)address; return 0;
}
void superfx_cpu_write_io(SuperFx *fx, uint16_t address, uint8_t data) {
    (void)fx; (void)address; (void)data;
}
uint8_t superfx_cpu_read_rom(SuperFx *fx, uint32_t address, uint8_t open_bus) {
    (void)fx; (void)address; return open_bus;
}
uint8_t superfx_cpu_read_ram(SuperFx *fx, uint32_t address, uint8_t open_bus) {
    (void)fx; (void)address; return open_bus;
}
void superfx_cpu_write_ram(SuperFx *fx, uint32_t address, uint8_t data) {
    (void)fx; (void)address; (void)data;
}
void debug_on_wram_write_byte(uint32 addr, uint8 old_value, uint8 new_value) {
    (void)addr; (void)old_value; (void)new_value;
}
void debug_on_wram_write_word(uint32 addr, uint16 old_value, uint16 new_value) {
    (void)addr; (void)old_value; (void)new_value;
}

static RecompReturn fake_aot(CpuState *cpu) {
    g_aot_calls++;
    cpu->A++;
    return RECOMP_RETURN_NORMAL;
}

const DispatchEntry g_dispatch_table[] = {
    {0x008100u, {fake_aot, NULL, NULL, NULL}, 0},
    {0x008200u, {NULL, NULL, NULL, NULL}, 0},
};
const unsigned g_dispatch_table_count =
    (unsigned)(sizeof g_dispatch_table / sizeof g_dispatch_table[0]);

RecompReturn interp_tier_dispatch_popped_return(
    CpuState *cpu, uint32 target_pc24, uint32 site_pc24,
    uint16 miss_restore_s) {
    g_lle_calls++;
    g_lle_target = target_pc24;
    g_lle_site = site_pc24;
    g_lle_restore_s = miss_restore_s;
    cpu->A += 0x100;
    cpu->S += 2;  /* model the interpreted target consuming its RTS frame */
    return RECOMP_RETURN_NORMAL;
}

RecompReturn interp_tier_run_call(CpuState *cpu, uint32 target_pc24,
                                  uint32 source_pc24) {
    (void)cpu; (void)target_pc24; (void)source_pc24;
    return RECOMP_RETURN_NORMAL;
}
RecompReturn interp_tier_run_call_frame(CpuState *cpu, uint32 target_pc24,
                                        uint32 source_pc24, uint8 frame_size,
                                        uint32 *return_pc24) {
    (void)cpu; (void)target_pc24; (void)source_pc24;
    (void)frame_size; (void)return_pc24;
    return RECOMP_RETURN_NORMAL;
}

static int check(int cond, const char *what) {
    if (!cond) fprintf(stderr, "FAIL: %s\n", what);
    return cond ? 0 : 1;
}

int main(void) {
    CpuState cpu;
    Snes snes;
    Cart cart;
    uint8 ram[0x20000];
    uint8 sram[0x8000];
    int fails = 0;

    memset(&snes, 0, sizeof snes);
    memset(&cart, 0, sizeof cart);
    memset(ram, 0, sizeof ram);
    memset(sram, 0, sizeof sram);
    snes.cart = &cart;
    cart.rom = g_test_rom;
    cart.romSize = (int)sizeof g_test_rom;
    cart.ram = sram;
    cart.ramSize = (int)sizeof sram;
    g_snes = &snes;
    g_sram = sram;
    g_sram_size = (int)sizeof sram;
    cpu_state_init(&cpu, ram);

    /* $F0:0000-$7FFF is LoROM SRAM but full-address HiROM ROM. The runtime
     * must select only the active cartridge's SRAM window. */
    g_test_rom[0x3009fe] = 0x1c;
    g_test_rom[0x3009ff] = 0x00;
    sram[0x09fe] = 0x34;
    sram[0x09ff] = 0x12;
    cart.type = CART_HIROM;
    fails += check(cpu_read16(&cpu, 0xf0, 0x09fe) == 0x001c,
                   "HiROM low address reads ROM, not LoROM SRAM");
    cart.type = CART_LOROM;
    fails += check(cpu_read16(&cpu, 0xf0, 0x09fe) == 0x1234,
                   "LoROM low address still reads SRAM");
    fails += check(cart_getRomPtr(&cart, 0xf0, 0x09fe) == NULL,
                   "LoROM direct ROM pointer rejects active SRAM window");

    cart.type = CART_HIROM;
    fails += check(cart_getRomPtr(&cart, 0xc1, 0x1234) ==
                       g_test_rom + 0x11234,
                   "HiROM full-bank pointer uses 64 KiB bank stride");
    fails += check(cart_getRomPtr(&cart, 0x00, 0x2000) == NULL,
                   "HiROM pointer rejects low-bank I/O window");
    cart.type = CART_LOROM;
    fails += check(cart_getRomPtr(&cart, 0x00, 0x8000) == g_test_rom,
                   "LoROM pointer preserves historical half-bank mapping");

    cpu_state_init(&cpu, ram);
    cpu.S = 0x01f0;
    cpu.emulation = false;
    cpu.P = 0x34;
    cpu_p_to_mirrors(&cpu);
    cpu_push_interrupt_frame_at(&cpu, 0xabcdef);
    fails += check(cpu.S == 0x01ec, "native interrupt frame consumes four bytes");
    fails += check(ram[0x01f0] == 0xab && ram[0x01ef] == 0xcd &&
                       ram[0x01ee] == 0xef,
                   "interrupt frame preserves supplied 24-bit return PC");
    fails += check(ram[0x01ed] == 0x34,
                   "interrupt frame materializes processor status");

    memset(&cpu, 0, sizeof cpu);
    cpu.S = 0x1F0;
    fails += check(cpu_dispatch_pc_from(&cpu, 0x008100u, 0x1F2, 0x008000u)
                   == RECOMP_RETURN_NORMAL, "AOT return");
    fails += check(g_aot_calls == 1 && g_lle_calls == 0, "exact AOT selected");
    fails += check(cpu.PB == 0x00 && cpu.A == 1, "AOT architectural state");

    memset(&cpu, 0, sizeof cpu);
    cpu.S = 0x1E0;
    fails += check(cpu_dispatch_pc_from(&cpu, 0x008200u, 0x1E5, 0x008010u)
                   == RECOMP_RETURN_NORMAL, "LLE return");
    fails += check(g_lle_calls == 1, "known NULL row selected LLE");
    fails += check(g_lle_target == 0x008200u && g_lle_site == 0x008010u,
                   "LLE target and source");
    fails += check(g_lle_restore_s == 0x1E5, "LLE receives safe bail restore");
    fails += check(cpu.S == 0x1E2 && cpu.A == 0x100, "LLE effects preserved");

    memset(&cpu, 0, sizeof cpu);
    cpu.S = 0x1D0;
    fails += check(cpu_dispatch_pc_from(&cpu, 0x008300u, 0x1D7, 0x008020u)
                   == RECOMP_RETURN_NORMAL, "unknown return");
    fails += check(g_lle_calls == 1, "unknown continuation does not run LLE");
    fails += check(cpu.S == 0x1D7, "unknown continuation restores stack");

    memset(&cpu, 0, sizeof cpu);
    cpu.S = 0x1C0;
    fails += check(cpu_dispatch_pc_from(&cpu, 0x808100u, 0x1C2, 0x808000u)
                   == RECOMP_RETURN_NORMAL, "mirror return");
    fails += check(g_aot_calls == 2, "LoROM mirror finds exact AOT");
    fails += check(cpu.PB == 0x80, "requested mirror bank remains architectural PB");

    if (fails) return 1;
    puts("known_lle_entry_test: PASS");
    return 0;
}
