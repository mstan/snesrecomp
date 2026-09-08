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
const char *g_last_recomp_func = "wlog_addr_lazy_test";
uint8 *g_sram;
int g_sram_size;
uint64_t g_main_cpu_cycles_estimate;
uint64_t g_apu_pace_cycles_estimate;
Snes *g_snes;
const RamRoutineGuard g_ram_routine_guards[] = {{0}};
const unsigned g_ram_routine_guard_count = 0;
const DispatchEntry g_dispatch_table[] = {{0}};
const unsigned g_dispatch_table_count = 0;

static const char *g_mode;
static int g_wlog_addr_getenv_calls;

char *__wrap_getenv(const char *name) {
    if (strcmp(name, "SNESRECOMP_WLOG_ADDR") == 0) {
        g_wlog_addr_getenv_calls++;
        if (strcmp(g_mode, "unset") == 0)
            return NULL;
        if (strcmp(g_mode, "empty") == 0)
            return "";
        if (strcmp(g_mode, "invalid") == 0)
            return "not-a-valid-wlog-spec";
    }
    return NULL;
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
    (void)cart; (void)bank; (void)addr;
}
void cart_write(Cart *cart, uint8 bank, uint16 addr, uint8 value) {
    (void)cart; (void)bank; (void)addr; (void)value;
}
uint8 cart_read(Cart *cart, uint8 bank, uint16 addr) {
    (void)cart; (void)bank; (void)addr; return 0;
}
uint8 *cart_getRomPtr(Cart *cart, uint8 bank, uint16 addr) {
    (void)cart; (void)bank; (void)addr; return NULL;
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

static int valid_mode(const char *mode) {
    return strcmp(mode, "unset") == 0 || strcmp(mode, "empty") == 0 ||
           strcmp(mode, "invalid") == 0;
}

int main(int argc, char **argv) {
    CpuState cpu;
    uint8 ram[0x20000];

    if (argc != 2 || !valid_mode(argv[1])) {
        fprintf(stderr, "usage: wlog_addr_lazy_test unset|empty|invalid\n");
        return 2;
    }

    g_mode = argv[1];
    memset(&cpu, 0, sizeof(cpu));
    memset(ram, 0, sizeof(ram));
    cpu.ram = ram;

    for (int i = 0; i < 1000; ++i) {
        cpu_write8(&cpu, 0x7e, (uint16)i, (uint8)i);
        cpu_write16(&cpu, 0x7e, (uint16)(0x1000 + i), (uint16)i);
    }

    if (g_wlog_addr_getenv_calls != 1) {
        fprintf(stderr, "FAIL: %s called getenv %d times\n", g_mode,
                g_wlog_addr_getenv_calls);
        return 1;
    }

    printf("wlog_addr_lazy_test %s: PASS\n", g_mode);
    return 0;
}
