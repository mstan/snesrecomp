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
 *   $00-$7D:8000-$FFFF / $80-$FF:8000-$FFFF   -> ROM (reads via RomPtr;
 *                                                writes are NOPs)
 *
 * The hardware-register routing is what unblocks boot: every PPU/APU/DMA
 * register write the recompiled code emits goes through WriteReg, so
 * INIDISP / NMITIMEN / OBSEL / DMA setup actually take effect. Without
 * it, $2100 stays at the snes9x default (forced-blank ON) and the
 * screen never lights up.
 */

#include "cpu_state.h"
#include "common_rtl.h"

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
static inline void cpu_pace_cycles(void) {
    g_main_cpu_cycles_estimate += 24;
}

/* DEBUG: tally HW-reg accesses + last-touched address so we can tell
 * whether a hang is "spinning thousands of polls" vs "stuck in a non-
 * HW-reg loop" without having to launch + attach. Counts get printed
 * on a millionth-access basis. */
static uint64_t s_hw_touch_count = 0;
static uint16 s_last_hw_addr = 0;
static int s_last_hw_was_read = 0;
static int s_apu_writes_logged = 0;
static int s_apu_writes_summary_at_2k = 0;
static void cpu_hw_log(uint16 addr, int is_read, uint16 val) {
    s_last_hw_addr = addr;
    s_last_hw_was_read = is_read;
    /* Log first 50 APU writes verbatim; then every 2000th. */
    if (!is_read && addr >= 0x2140 && addr <= 0x2143) {
        s_apu_writes_logged++;
        if (s_apu_writes_logged <= 50 ||
            (s_apu_writes_logged - 50) >= s_apu_writes_summary_at_2k * 2000) {
            if (s_apu_writes_logged > 50) s_apu_writes_summary_at_2k++;
            fprintf(stderr, "[apu-write #%d] $%04X = $%02X (touch=%llu)\n",
                    s_apu_writes_logged, addr, val & 0xFF,
                    (unsigned long long)s_hw_touch_count);
            fflush(stderr);
        }
    }
    if (++s_hw_touch_count % 1000000 == 0) {
        fprintf(stderr, "[hw-pace] touches=%llu apu-writes=%d last=%c$%04X $2140=%02X $2141=%02X $2142=%02X $2143=%02X\n",
                (unsigned long long)s_hw_touch_count, s_apu_writes_logged,
                is_read ? 'R' : 'W', addr,
                ReadReg(0x2140), ReadReg(0x2141), ReadReg(0x2142), ReadReg(0x2143));
        fflush(stderr);
    }
}

uint8 cpu_read8(CpuState *cpu, uint8 bank, uint16 addr) {
    int off = cpu_ram_offset(bank, addr);
    if (off >= 0) return cpu->ram[off];
    if (is_hw_reg(bank, addr)) { cpu_pace_cycles(); cpu_hw_log(addr, 1, 0); return ReadReg(addr); }
    /* ROM read. RomPtr requires the global g_rom pointer to be live. */
    return *RomPtr(((uint32)bank << 16) | addr);
}

uint16 cpu_read16(CpuState *cpu, uint8 bank, uint16 addr) {
    int off = cpu_ram_offset(bank, addr);
    if (off >= 0 && off + 1 < 0x20000)
        return (uint16)cpu->ram[off] | ((uint16)cpu->ram[off + 1] << 8);
    if (is_hw_reg(bank, addr)) { cpu_pace_cycles(); cpu_hw_log(addr, 1, 0); return ReadRegWord(addr); }
    /* ROM word read. */
    const uint8 *p = RomPtr(((uint32)bank << 16) | addr);
    return (uint16)p[0] | ((uint16)p[1] << 8);
}

void cpu_write8(CpuState *cpu, uint8 bank, uint16 addr, uint8 v) {
    int off = cpu_ram_offset(bank, addr);
    if (off >= 0) { cpu->ram[off] = v; return; }
    if (is_hw_reg(bank, addr)) { cpu_pace_cycles(); cpu_hw_log(addr, 0, v); WriteReg(addr, v); return; }
    /* ROM / unmapped write: drop. */
}

void cpu_write16(CpuState *cpu, uint8 bank, uint16 addr, uint16 v) {
    int off = cpu_ram_offset(bank, addr);
    if (off >= 0 && off + 1 < 0x20000) {
        cpu->ram[off]     = (uint8)(v & 0xFF);
        cpu->ram[off + 1] = (uint8)(v >> 8);
        return;
    }
    if (is_hw_reg(bank, addr)) { cpu_pace_cycles(); cpu_hw_log(addr, 0, v); WriteRegWord(addr, v); return; }
    /* ROM / unmapped write: drop. */
}

void cpu_state_init(CpuState *cpu, uint8 *ram) {
    cpu->A = 0;
    cpu->B = 0;
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
}
