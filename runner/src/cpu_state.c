/*
 * cpu_state.c — implementations for the v2 runtime CpuState.
 *
 * The byte/word memory helpers map (bank, addr) onto the existing
 * `g_ram[0x20000]` layout used by the hand-written runtime:
 *   $00-$3F:8000-$00-$3F:FFFF  -> ROM (handled elsewhere; not via cpu_*)
 *   $7E:0000-$7E:FFFF          -> g_ram[0x00000-0x0FFFF]
 *   $7F:0000-$7F:FFFF          -> g_ram[0x10000-0x1FFFF]
 *   $00-$3F:0000-$00-$3F:1FFF  -> WRAM mirror of $7E:0000-$7E:1FFF
 *
 * Phase 4 ships just the byte/word primitives. Phase 5 wires in the
 * higher-level segment-resolution helpers (DB+abs, D+dp, S+offs,
 * indirect-Y, indirect-long, etc.).
 */

#include "cpu_state.h"

CpuState g_cpu;

/* Map a 24-bit logical address onto a g_ram offset (or return -1 for
 * non-WRAM ranges — those are handled by ROM / hardware-register paths
 * elsewhere in the runtime). */
static int cpu_ram_offset(uint8 bank, uint16 addr) {
    /* Direct WRAM banks. */
    if (bank == 0x7E) return (int)addr;
    if (bank == 0x7F) return 0x10000 + (int)addr;
    /* WRAM mirror in low banks $00-$3F and $80-$BF, addresses $0000-$1FFF. */
    if (addr < 0x2000 && (bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF))) {
        return (int)addr;
    }
    return -1;
}

uint8 cpu_read8(CpuState *cpu, uint8 bank, uint16 addr) {
    int off = cpu_ram_offset(bank, addr);
    if (off < 0) return 0;
    return cpu->ram[off];
}

uint16 cpu_read16(CpuState *cpu, uint8 bank, uint16 addr) {
    int off = cpu_ram_offset(bank, addr);
    if (off < 0 || off + 1 >= 0x20000) return 0;
    return (uint16)cpu->ram[off] | ((uint16)cpu->ram[off + 1] << 8);
}

void cpu_write8(CpuState *cpu, uint8 bank, uint16 addr, uint8 v) {
    int off = cpu_ram_offset(bank, addr);
    if (off < 0) return;
    cpu->ram[off] = v;
}

void cpu_write16(CpuState *cpu, uint8 bank, uint16 addr, uint16 v) {
    int off = cpu_ram_offset(bank, addr);
    if (off < 0 || off + 1 >= 0x20000) return;
    cpu->ram[off]     = (uint8)(v & 0xFF);
    cpu->ram[off + 1] = (uint8)(v >> 8);
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
    cpu->ram = ram;
}
