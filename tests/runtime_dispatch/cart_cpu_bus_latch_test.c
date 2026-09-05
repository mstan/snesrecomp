#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cpu_state.h"
#include "snes/cart.h"
#include "snes/snes.h"
#include "snes/superfx.h"

uint32_t g_interp_wlog_pc24 = 0;
uint32_t g_interp816_cur_pc = 0;
uint8 g_memsel;
int snes_frame_counter;
const char *g_last_recomp_func = "cart_cpu_bus_latch_test";
uint8 *g_sram;
int g_sram_size;
uint64_t g_main_cpu_cycles_estimate;
uint64_t g_apu_pace_cycles_estimate;
Snes *g_snes;
const RamRoutineGuard g_ram_routine_guards[] = {{0}};
const unsigned g_ram_routine_guard_count = 0;
const DispatchEntry g_dispatch_table[] = {{0}};
const unsigned g_dispatch_table_count = 0;

static int g_note_calls;
static int g_failures;
static uint32_t g_expected_latch;
static int g_expect_slow_path_latch;
static int g_cart_read_calls;
static int g_cart_write_calls;

static void check_int(const char *what, int got, int want) {
    if (got != want) {
        fprintf(stderr, "FAIL: %s got %d want %d\n", what, got, want);
        g_failures++;
    }
}

static void check_u32(const char *what, uint32_t got, uint32_t want) {
    if (got != want) {
        fprintf(stderr, "FAIL: %s got 0x%06x want 0x%06x\n", what,
                (unsigned)got, (unsigned)want);
        g_failures++;
    }
}

uint8 ReadReg(uint16 reg) { (void)reg; return 0; }
uint16 ReadRegWord(uint16 reg) { (void)reg; return 0; }
uint8 ReadRegOpenBus(uint16 reg, uint8 open_bus) {
    (void)reg;
    return open_bus;
}
void WriteReg(uint16 reg, uint8 value) { (void)reg; (void)value; }
void WriteRegWord(uint16 reg, uint16 value) { (void)reg; (void)value; }
uint8 *RomPtr(uint32 addr) { (void)addr; return NULL; }

void cart_note_cpu_bus(Cart *cart, uint8 bank, uint16 addr) {
    g_note_calls++;
    cart->cpuBusAddress = ((uint32_t)bank << 16) | addr;
}

uint8 cart_read(Cart *cart, uint8 bank, uint16 addr) {
    if (g_expect_slow_path_latch && g_cart_read_calls == 0) {
        check_u32("SA-1 read slow-path latch observed before cart_read",
                  cart->cpuBusAddress, g_expected_latch);
    }
    g_cart_read_calls++;
    (void)bank;
    return (uint8)(addr & 0xffu);
}

void cart_write(Cart *cart, uint8 bank, uint16 addr, uint8 value) {
    if (g_expect_slow_path_latch && g_cart_write_calls == 0) {
        check_u32("SA-1 write slow-path latch observed before cart_write",
                  cart->cpuBusAddress, g_expected_latch);
    }
    g_cart_write_calls++;
    (void)bank;
    (void)addr;
    (void)value;
}

uint8 *cart_getRomPtr(Cart *cart, uint8 bank, uint16 addr) {
    (void)cart;
    (void)bank;
    (void)addr;
    return NULL;
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
void cpu_trace_wram_write_check(CpuState *cpu, uint8 bank, uint16 addr,
                                int off, uint16 old_value, uint16 new_value,
                                int width) {
    (void)cpu; (void)bank; (void)addr; (void)off;
    (void)old_value; (void)new_value; (void)width;
}
RecompReturn interp_tier_dispatch_popped_return(
    CpuState *cpu, uint32 target_pc24, uint32 site_pc24,
    uint16 miss_restore_s) {
    (void)cpu; (void)target_pc24; (void)site_pc24; (void)miss_restore_s;
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

static void reset_cpu(CpuState *cpu, uint8 *ram) {
    memset(cpu, 0, sizeof(*cpu));
    memset(ram, 0, 0x20000);
    cpu->ram = ram;
    g_sram = NULL;
    g_sram_size = 0;
}

static void exercise_non_sa1(void) {
    Snes snes;
    Cart cart;
    CpuState cpu;
    uint8 ram[0x20000];

    memset(&snes, 0, sizeof(snes));
    memset(&cart, 0, sizeof(cart));
    reset_cpu(&cpu, ram);
    g_snes = &snes;
    snes.cart = &cart;
    cart.type = CART_LOROM;
    cart.cpuBusAddress = 0x00abcdefu;

    ram[0x1234] = 0x5a;
    check_int("non-SA1 read8 value", cpu_read8(&cpu, 0x7e, 0x1234), 0x5a);
    check_int("non-SA1 read8 open_bus", cpu.open_bus, 0x5a);
    cpu_write8(&cpu, 0x7e, 0x1235, 0x66);
    check_int("non-SA1 write8 RAM", ram[0x1235], 0x66);
    ram[0x1240] = 0x34;
    ram[0x1241] = 0x12;
    check_int("non-SA1 read16 value", cpu_read16(&cpu, 0x7e, 0x1240),
              0x1234);
    cpu_write16(&cpu, 0x7e, 0x1242, 0x89ab);
    check_int("non-SA1 write16 low", ram[0x1242], 0xab);
    check_int("non-SA1 write16 high", ram[0x1243], 0x89);
}

static void exercise_sa1(void) {
    Snes snes;
    Cart cart;
    CpuState cpu;
    uint8 ram[0x20000];

    memset(&snes, 0, sizeof(snes));
    memset(&cart, 0, sizeof(cart));
    reset_cpu(&cpu, ram);
    g_snes = &snes;
    snes.cart = &cart;
    cart.type = CART_SA1;
    cart.sa1 = (Sa1 *)(uintptr_t)1;

    g_expected_latch = 0x008000u;
    g_expect_slow_path_latch = 1;
    check_int("SA-1 read8 value", cpu_read8(&cpu, 0x00, 0x8000), 0x00);
    check_u32("SA-1 read8 final latch", cart.cpuBusAddress, 0x008000u);

    g_expected_latch = 0x008010u;
    g_cart_read_calls = 0;
    check_int("SA-1 read16 value", cpu_read16(&cpu, 0x00, 0x8010),
              0x1110);
    check_u32("SA-1 read16 low-address latch", cart.cpuBusAddress,
              0x008010u);

    g_expected_latch = 0x008020u;
    g_cart_write_calls = 0;
    cpu_write8(&cpu, 0x00, 0x8020, 0x44);
    check_u32("SA-1 write8 final latch", cart.cpuBusAddress, 0x008020u);

    g_expected_latch = 0x008030u;
    g_cart_write_calls = 0;
    cpu_write16(&cpu, 0x00, 0x8030, 0x6655);
    check_u32("SA-1 write16 low-address latch", cart.cpuBusAddress,
              0x008030u);

    check_int("SA-1 cart_note calls", g_note_calls, 4);
    check_int("SA-1 read16 cart_read calls", g_cart_read_calls, 2);
    check_int("SA-1 write16 cart_write calls", g_cart_write_calls, 2);
}

int main(void) {
    exercise_non_sa1();

    g_note_calls = 0;
    g_cart_read_calls = 0;
    g_cart_write_calls = 0;
    exercise_sa1();

    if (g_failures) return 1;
    puts("cart_cpu_bus_latch_test: PASS");
    return 0;
}
