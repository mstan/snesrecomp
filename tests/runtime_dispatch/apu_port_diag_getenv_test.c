#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "interp_bridge.h"
#include "interp816.h"
#include "snes.h"
#include "sa1.h"
#include "snes_cycles.h"

CpuState g_cpu;
static Snes g_test_snes;
Snes *g_snes = &g_test_snes;
uint64_t g_apu_last_sync_master;
int g_interp_apu_driving;
int snes_frame_counter;
uint8 g_memsel;
int g_recomp_stack_top;
uint16_t g_cpu_entry_s[64];

static uint8_t g_ram[0x10000];
static const char *g_mode;
static int g_apu_port_diag_getenv_calls;
static int g_apu_catchup_calls;

extern void interp_bridge_test_apu_port_diag_probe(CpuState *cpu, int apu_port);

char *__wrap_getenv(const char *name) {
    if (strcmp(name, "SNESRECOMP_APU_PORT_DIAG") == 0) {
        g_apu_port_diag_getenv_calls++;
        if (!g_mode)
            return NULL;
        if (strncmp(g_mode, "unset-", 6) == 0)
            return NULL;
        if (strncmp(g_mode, "empty-", 6) == 0)
            return "";
        if (strncmp(g_mode, "zero-", 5) == 0)
            return "0";
    }
    return NULL;
}

const char *rtl_game_title(void) { return "apu_port_diag_getenv_test"; }
bool rtl_apu_frame_timeline_active(void) { return false; }
bool sa1_cpu_irq_pending(const Sa1 *sa1) { (void)sa1; return false; }
void RtlApuLock(void) {}
void RtlApuUnlock(void) {}
void rtl_sync_apu_to_cpu_locked(void) {}
void snes_catchupApu(Snes *snes) {
    (void)snes;
    g_apu_catchup_calls++;
}
void snes_sync_master_clock(Snes *snes, uint64_t master_clock) {
    (void)snes;
    (void)master_clock;
}
uint8 cpu_read8(CpuState *cpu, uint8 bank, uint16 addr) {
    (void)cpu;
    (void)bank;
    return g_ram[addr];
}
void cpu_write8(CpuState *cpu, uint8 bank, uint16 addr, uint8 value) {
    (void)cpu;
    (void)bank;
    g_ram[addr] = value;
}
uint16 cpu_read16(CpuState *cpu, uint8 bank, uint16 addr) {
    uint16 lo = cpu_read8(cpu, bank, addr);
    uint16 hi = cpu_read8(cpu, bank, (uint16)(addr + 1));
    return (uint16)(lo | (uint16)(hi << 8));
}
void cpu_write16(CpuState *cpu, uint8 bank, uint16 addr, uint16 value) {
    cpu_write8(cpu, bank, addr, (uint8)value);
    cpu_write8(cpu, bank, (uint16)(addr + 1), (uint8)(value >> 8));
}
uint32_t cpu_region_speed(uint32_t addr24) {
    return (uint32_t)snes_region_speed(addr24, g_memsel);
}
void debug_on_block_enter(uint32_t pc, uint32_t a, uint32_t x, uint32_t y) {
    (void)pc;
    (void)a;
    (void)x;
    (void)y;
}
int cx4_irq_pending(const Cx4 *cx4) { (void)cx4; return 0; }
void cart_sync_coprocessors(Cart *cart, uint64_t master_clock) {
    (void)cart;
    (void)master_clock;
}
uint8_t sdd1_read(Sdd1 *sdd1, uint16_t addr) {
    (void)sdd1;
    (void)addr;
    return 0;
}
int interp816_runOpcode(Interp816 *cpu) { (void)cpu; return 0; }
int cpu_take_tailcall_return_context(uint16_t *entry_s, uint8_t *hrv) {
    (void)entry_s;
    (void)hrv;
    return 0;
}
int cpu_dispatch_has_entry(CpuState *cpu, uint32 pc24) {
    (void)cpu;
    (void)pc24;
    return 0;
}
RecompReturn cpu_dispatch_pc_paired(CpuState *cpu, uint32 pc24,
                                    uint8 frame_size) {
    (void)pc24;
    cpu->S = (uint16)(cpu->S + frame_size);
    return RECOMP_RETURN_NORMAL;
}
uint8 cpu_dispatch_inline_arg_bytes(uint32 pc24) {
    (void)pc24;
    return 0;
}
void cpu_interrupt_context_enter(void) {}
void cpu_interrupt_context_leave(void) {}
int cpu_interrupt_context_active(void) { return 0; }
void wlog_scope_enter(const char *tag) { (void)tag; }
void wlog_scope_exit(void) {}
int cpu_resolve_post_return_skip(uint16_t post_s) {
    (void)post_s;
    return 0;
}
RecompReturn cpu_unresolved_abandon_balanced(CpuState *cpu, uint32 site_pc24,
                                             uint16 entry_s, uint8 hrv) {
    (void)site_pc24;
    cpu->S = (uint16)(entry_s + hrv);
    return RECOMP_RETURN_NORMAL;
}
int tier2_capture_enabled(void) { return 0; }
const char *tier2_capture_manifest_path(const char *rom_title) {
    (void)rom_title;
    return NULL;
}
int tier2_capture_append_discovery(const char *rom_title,
                                   uint32_t site_pc24,
                                   uint32_t target_pc24,
                                   const char *entry_mx,
                                   const char *site_kind,
                                   int outcome,
                                   int32_t frame) {
    (void)rom_title;
    (void)site_pc24;
    (void)target_pc24;
    (void)entry_mx;
    (void)site_kind;
    (void)outcome;
    (void)frame;
    return 1;
}

static int mode_is_apu_port(const char *mode) {
    const char *dash = strrchr(mode, '-');
    return dash && strcmp(dash + 1, "apu") == 0;
}

static int valid_mode(const char *mode) {
    return strcmp(mode, "unset-apu") == 0 ||
           strcmp(mode, "empty-apu") == 0 ||
           strcmp(mode, "zero-apu") == 0 ||
           strcmp(mode, "unset-nonport") == 0 ||
           strcmp(mode, "empty-nonport") == 0 ||
           strcmp(mode, "zero-nonport") == 0;
}

static int check_int(const char *what, int got, int want) {
    if (got == want)
        return 0;
    fprintf(stderr, "FAIL: %s got %d want %d\n", what, got, want);
    return 1;
}

int main(int argc, char **argv) {
    int failures = 0;

    if (argc != 2 || !valid_mode(argv[1])) {
        fprintf(stderr,
                "usage: apu_port_diag_getenv_test "
                "unset-apu|empty-apu|zero-apu|"
                "unset-nonport|empty-nonport|zero-nonport\n");
        return 2;
    }

    g_mode = argv[1];
    memset(&g_cpu, 0, sizeof(g_cpu));
    memset(&g_test_snes, 0, sizeof(g_test_snes));
    memset(g_ram, 0, sizeof(g_ram));
    g_cpu.master_cycles = 12345;

    const int is_apu_port = mode_is_apu_port(g_mode);
    interp_bridge_test_apu_port_diag_probe(&g_cpu, is_apu_port);

    failures += check_int("getenv calls", g_apu_port_diag_getenv_calls,
                          is_apu_port ? 1 : 0);
    failures += check_int("APU catchup calls", g_apu_catchup_calls,
                          is_apu_port ? 1 : 0);
    if (is_apu_port)
        failures += check_int("last APU sync master",
                              (int)g_apu_last_sync_master, 12345);

    if (failures)
        return 1;
    printf("apu_port_diag_getenv_test %s: PASS\n", g_mode);
    return 0;
}
