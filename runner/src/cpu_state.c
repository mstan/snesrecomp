/*
 * cpu_state.c — implementations for the v2 runtime CpuState.
 *
 * Address routing for the byte/word memory helpers:
 *   $00-$3F:0000-$1FFF / $7E:0000-$1FFF       -> g_ram (low WRAM mirror)
 *   $7E:0000-$FFFF                            -> g_ram[0x00000-0x0FFFF]
 *   $7F:0000-$FFFF                            -> g_ram[0x10000-0x1FFFF]
 *   $00-$3F:2000-$5FFF / $80-$BF:2000-$5FFF   -> SNES hardware regs
 *                                                (PPU, APU, joypad, DMA)
 *                                                routed via WriteReg/ReadReg
 *   $70-$7D:0000-$7FFF / $F0-$FD:0000-$7FFF   -> LoROM battery SRAM
 *                                                (cart->ram via g_sram)
 *   $00-$3F:6000-$7FFF / $80-$BF:6000-$7FFF   -> HiROM battery SRAM
 *                                                (cart->ram via g_sram)
 *   $00-$7D:8000-$FFFF / $80-$FF:8000-$FFFF   -> ROM (reads via RomPtr;
 *                                                writes are NOPs)
 *
 * The hardware-register routing is what unblocks boot: every PPU/APU/DMA
 * register write the recompiled code emits goes through WriteReg, so
 * INIDISP / NMITIMEN / OBSEL / DMA setup actually take effect. Without
 * it, $2100 stays at the snes9x default (forced-blank ON) and the
 * screen never lights up.
 *
 * The SRAM routing is what unblocks save/menu: every read against the
 * cart's battery RAM (SMW's VerifySaveFile, save data writes, password
 * tables, etc.) goes through g_sram so save data lives in cart->ram
 * instead of tripping RomPtr-invalid.
 */

#include "cpu_state.h"
#include "common_rtl.h"
#include "cpu_trace.h"

CpuState g_cpu;

/* Map a 24-bit logical address onto a g_ram offset. Returns -1 for
 * addresses that are NOT WRAM — the caller routes those to the HW-reg
 * helpers (WriteReg/ReadReg) or to ROM. */
static int cpu_ram_offset(uint8 bank, uint16 addr) {
    if (bank == 0x7E) return (int)addr;
    if (bank == 0x7F) return 0x10000 + (int)addr;
    if (addr < 0x2000 && (bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF))) {
        return (int)addr;
    }
    return -1;
}

/* True when (bank, addr) addresses an SNES hardware register that should
 * be routed through the framework's WriteReg/ReadReg dispatch. The HW
 * register window is $2000-$5FFF in low banks ($00-$3F, $80-$BF). */
static int is_hw_reg(uint8 bank, uint16 addr) {
    if (addr < 0x2000 || addr >= 0x6000) return 0;
    if (bank <= 0x3F) return 1;
    if (bank >= 0x80 && bank <= 0xBF) return 1;
    return 0;
}

/* Map a 24-bit logical address onto a g_sram offset for cart battery
 * RAM. Returns -1 if (bank, addr) is NOT SRAM. Mirrors snes9x's
 * cart_readLorom and cart_readHirom SRAM mappings so save-data
 * accesses route to cart->ram instead of falling through to RomPtr
 * (which would trip the RomPtr-invalid off-rails detector). */
static int cpu_sram_offset(uint8 bank, uint16 addr) {
    if (g_sram_size == 0 || g_sram == NULL) return -1;
    /* LoROM SRAM: banks $70-$7D + $F0-$FD, addr $0000-$7FFF. */
    if (((bank >= 0x70 && bank < 0x7E) || (bank >= 0xF0 && bank < 0xFE))
        && addr < 0x8000) {
        return (int)((((bank & 0xF) << 15) | addr) & (g_sram_size - 1));
    }
    /* HiROM SRAM: banks $00-$3F + $80-$BF, addr $6000-$7FFF. */
    if ((bank < 0x40 || (bank >= 0x80 && bank < 0xC0))
        && addr >= 0x6000 && addr < 0x8000) {
        return (int)((((bank & 0x3F) << 13) | (addr & 0x1FFF))
                     & (g_sram_size - 1));
    }
    return -1;
}

/* APU pacing: every HW-register touch advances the main-CPU cycle
 * estimate. v1 did this in `debug_on_block_enter` (RDB_BLOCK_HOOK); v2
 * doesn't emit those, so without this bump g_main_cpu_cycles_estimate
 * stays at 0, snes_catchupApu never advances the SPC, and SMW's
 * "wait for $2140 == $BBAA" poll loop spins forever waiting for a
 * response that the APU can't produce.
 *
 * Per-touch granularity is overshooting reality (real CPU does ~6
 * cycles per insn, far less than 24 per touch) but the SPC handshake
 * doesn't care about precise timing — it just needs *some* cycles to
 * elapse so the IPL ROM runs to the point of writing $BBAA. */
#include <stdio.h>
/* APU pacing: every HW-register touch advances the main-CPU cycle
 * estimate. v1 did this in `debug_on_block_enter`; v2 doesn't emit
 * those, so without this bump the SPC never advances and SMW's
 * "wait for $2140 == $BBAA" handshake spins forever.
 *
 * The 256-cycle increment is tuned to roughly match v1's per-block
 * pacing amortised over the recomp's tight CPU read loops. The
 * minimum-cycle floor in snes_catchupApu (snes.c) ensures the SPC
 * actually progresses on each call. */
static inline void cpu_pace_cycles(void) {
    g_main_cpu_cycles_estimate += 256;
}

/* Optional debug — disabled in release. Set BUILD_CPU_HW_LOG=1 in the
 * build to enable verbose per-touch logging. */
#define BUILD_CPU_HW_LOG 0
static uint64_t s_hw_touch_count = 0;
static uint16 s_last_hw_addr = 0;
static int s_last_hw_was_read = 0;
static int s_apu_writes_logged = 0;

/* Logger reachable from generated code. Disabled at release. */
void cpu_dbg_funcname(const char *name) {
    (void)name;
#if BUILD_CPU_HW_LOG
    static int n = 0;
    if (n++ < 50) {
        fprintf(stderr, "[func#%d] %s (touch=%llu)\n",
                n, name, (unsigned long long)s_hw_touch_count);
        fflush(stderr);
    }
#endif
}
static void cpu_hw_log(uint16 addr, int is_read, uint16 val) {
    s_last_hw_addr = addr;
    s_last_hw_was_read = is_read;
    if (!is_read && addr >= 0x2140 && addr <= 0x2143) {
        s_apu_writes_logged++;
    }
    s_hw_touch_count++;
#if BUILD_CPU_HW_LOG
    (void)val;
    if (s_hw_touch_count % 1000000 == 0) {
        fprintf(stderr, "[hw-pace] touches=%llu\n", (unsigned long long)s_hw_touch_count);
        fflush(stderr);
    }
#else
    (void)val;
#endif
}

uint8 cpu_read8(CpuState *cpu, uint8 bank, uint16 addr) {
    int off = cpu_ram_offset(bank, addr);
    if (off >= 0) return cpu->ram[off];
    if (is_hw_reg(bank, addr)) { cpu_pace_cycles(); cpu_hw_log(addr, 1, 0); return ReadReg(addr); }
    int sram = cpu_sram_offset(bank, addr);
    if (sram >= 0) return g_sram[sram];
    /* ROM read. RomPtr requires the global g_rom pointer to be live. */
    return *RomPtr(((uint32)bank << 16) | addr);
}

uint16 cpu_read16(CpuState *cpu, uint8 bank, uint16 addr) {
    int off = cpu_ram_offset(bank, addr);
    if (off >= 0 && off + 1 < 0x20000)
        return (uint16)cpu->ram[off] | ((uint16)cpu->ram[off + 1] << 8);
    if (is_hw_reg(bank, addr)) { cpu_pace_cycles(); cpu_hw_log(addr, 1, 0); return ReadRegWord(addr); }
    int sram_lo = cpu_sram_offset(bank, addr);
    if (sram_lo >= 0) {
        /* Compose word from two byte fetches. If the high byte crosses
         * out of SRAM (e.g. word read at $70:$7FFF), fall through to
         * cpu_read8 for that byte so the boundary is handled by the
         * same routing logic. */
        int sram_hi = cpu_sram_offset(bank, (uint16)(addr + 1));
        uint8 hi = (sram_hi >= 0)
            ? g_sram[sram_hi]
            : cpu_read8(cpu, bank, (uint16)(addr + 1));
        return (uint16)g_sram[sram_lo] | ((uint16)hi << 8);
    }
    /* ROM word read. */
    const uint8 *p = RomPtr(((uint32)bank << 16) | addr);
    return (uint16)p[0] | ((uint16)p[1] << 8);
}

void cpu_write8(CpuState *cpu, uint8 bank, uint16 addr, uint8 v) {
    int off = cpu_ram_offset(bank, addr);
    if (off >= 0) {
        uint8 old = cpu->ram[off];
        cpu->ram[off] = v;
        cpu_trace_wram_write_check(cpu, bank, addr, off,
                                   (uint16)old, (uint16)v, 1);
        return;
    }
    if (is_hw_reg(bank, addr)) { cpu_pace_cycles(); cpu_hw_log(addr, 0, v); WriteReg(addr, v); return; }
    int sram = cpu_sram_offset(bank, addr);
    if (sram >= 0) { g_sram[sram] = v; return; }
    /* ROM / unmapped write: drop. */
}

void cpu_write16(CpuState *cpu, uint8 bank, uint16 addr, uint16 v) {
    int off = cpu_ram_offset(bank, addr);
    if (off >= 0 && off + 1 < 0x20000) {
        uint16 old = (uint16)cpu->ram[off]
                   | ((uint16)cpu->ram[off + 1] << 8);
        cpu->ram[off]     = (uint8)(v & 0xFF);
        cpu->ram[off + 1] = (uint8)(v >> 8);
        cpu_trace_wram_write_check(cpu, bank, addr, off, old, v, 2);
        return;
    }
    if (is_hw_reg(bank, addr)) { cpu_pace_cycles(); cpu_hw_log(addr, 0, v); WriteRegWord(addr, v); return; }
    int sram_lo = cpu_sram_offset(bank, addr);
    if (sram_lo >= 0) {
        g_sram[sram_lo] = (uint8)(v & 0xFF);
        int sram_hi = cpu_sram_offset(bank, (uint16)(addr + 1));
        if (sram_hi >= 0) g_sram[sram_hi] = (uint8)(v >> 8);
        else cpu_write8(cpu, bank, (uint16)(addr + 1), (uint8)(v >> 8));
        return;
    }
    /* ROM / unmapped write: drop. */
}

void cpu_state_init(CpuState *cpu, uint8 *ram) {
    cpu->A = 0;
    /* No cpu->B init — B is derived from (A >> 8) and has no separate state. */
    cpu->X = 0;
    cpu->Y = 0;
    cpu->S = 0x01FF;
    cpu->D = 0;
    cpu->DB = 0;
    cpu->PB = 0;
    /* Reset state per 65816 spec: emulation=1, M=X=I=1 (P=0x34). */
    cpu->P = CPU_P_M | CPU_P_X | CPU_P_I;
    cpu->m_flag = 1;
    cpu->x_flag = 1;
    cpu->emulation = 1;
    cpu->_flag_N = 0;
    cpu->_flag_V = 0;
    cpu->_flag_Z = 0;
    cpu->_flag_C = 0;
    cpu->_flag_I = 1;
    cpu->_flag_D = 0;
    cpu->ram = ram;
    /* NLR pending-skip is NOT on CpuState — it's a function-local in
     * each emitted v2 function. See cpu_state.h for design rationale. */
}
