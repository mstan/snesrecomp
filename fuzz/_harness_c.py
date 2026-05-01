"""Shared C harness fragments for the fuzz harnesses.

Each fuzz target builds a self-contained C harness. The boilerplate
splits cleanly into:

    BASE_PROLOGUE: types + g_ram + ReadReg/WriteReg stubs + WatchdogCheck.
                   Used by every fuzz harness.
    V1_PROLOGUE:   BASE_PROLOGUE + v1-style helpers (GET_WORD, PAIR16,
                   swap16, RomPtr_xx stubs). Required by v1's
                   recomp.emit_function output.
    V2_PROLOGUE:   BASE_PROLOGUE + the v2 CpuState struct and helpers
                   (cpu_p_to_mirrors, cpu_read*, cpu_write*, cpu_read_b,
                   cpu_trace_* stubs). Required by v2.codegen output.

The CpuState struct in V2_PROLOGUE inlines the layout from
runner/src/cpu_state.h. They MUST stay structurally compatible; the
v2 codegen emits direct field accesses (`cpu->A`, `cpu->_flag_Z`, etc.)
so any field rename in the real struct breaks the harness at build
time, which is the desired failure mode.
"""
from __future__ import annotations


# Bare types + WRAM + WatchdogCheck stub. All fuzz harnesses include this.
BASE_PROLOGUE = r"""
/* === fuzz BASE_PROLOGUE — shared types + g_ram + stubs === */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef uint8_t  uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef int8_t   int8;
typedef int16_t  int16;
typedef int32_t  int32;

/* 128 KB WRAM — banks $7E:$7F contiguous. */
static uint8_t g_ram[0x20000];

/* Minimal MMIO stubs. Real fuzz snippets shouldn't touch MMIO; if they
 * do, the test side will diverge from any oracle and the diff catches
 * it. Returning 0 / dropping writes means MMIO touches show up as
 * "everything is zero" rather than as crashes. */
static uint8_t  ReadReg(uint32_t a)        { (void)a; return 0; }
static uint16_t ReadRegWord(uint32_t a)    { (void)a; return 0; }
static void     WriteReg(uint32_t a, uint8_t v)     { (void)a; (void)v; }
static void     WriteRegWord(uint32_t a, uint16_t v){ (void)a; (void)v; }

/* Watchdog stub — generated bodies sometimes call it. */
static void WatchdogCheck(void) {}
"""


# v1 emitter helpers. Required by recomp.emit_function output.
V1_PROLOGUE = BASE_PROLOGUE + r"""
/* === fuzz V1_PROLOGUE — v1 emit-helper macros + RomPtr stubs === */

/* 16-bit-from-bytes helpers used in some v1 emits. */
#define GET_WORD(p) ((uint16)((uint8)(p)[0] | ((uint16)((uint8)(p)[1]) << 8)))
#define PAIR16(hi, lo) (((uint16)(uint8)(hi) << 8) | (uint8)(lo))
static inline uint16 swap16(uint16 v) {
    return (uint16)(((v & 0xff) << 8) | ((v >> 8) & 0xff));
}

/* Fuzz snippets don't read ROM; if a snippet's mode implicates ROM
 * the harness returns $FF (out-of-scope sentinel). The 7E/7F variants
 * point at WRAM, matching v1's bank routing. */
static const uint8_t s_rom_stub[4] = {0xff, 0xff, 0xff, 0xff};
static const uint8_t* RomPtr_00(uint32_t a) { (void)a; return s_rom_stub; }
static const uint8_t* RomPtr_7E(uint32_t a) { return &g_ram[a]; }
static const uint8_t* RomPtr_7F(uint32_t a) { return &g_ram[0x10000 + a]; }
"""


# v2 emitter helpers. Required by v2.codegen output.
#
# The CpuState struct here MUST mirror the field set of the real struct
# in runner/src/cpu_state.h. The compiler enforces this at fuzz build
# time: v2.codegen emits direct field accesses (`cpu->A` etc.), and any
# field rename or removal in the real struct breaks the harness build,
# which is exactly the warning we want.
V2_PROLOGUE = BASE_PROLOGUE + r"""
/* === fuzz V2_PROLOGUE — CpuState + cpu_read/write + flag mirrors === */

typedef struct CpuState {
    uint16 A;
    uint16 X;
    uint16 Y;
    uint16 S;
    uint16 D;
    uint8  DB;
    uint8  PB;
    uint8  P;
    uint8  m_flag;
    uint8  x_flag;
    uint8  emulation;
    uint8  _flag_N;
    uint8  _flag_V;
    uint8  _flag_Z;
    uint8  _flag_C;
    uint8  _flag_I;
    uint8  _flag_D;
    uint8 *ram;
} CpuState;

#define CPU_P_C 0x01u
#define CPU_P_Z 0x02u
#define CPU_P_I 0x04u
#define CPU_P_D 0x08u
#define CPU_P_X 0x10u
#define CPU_P_M 0x20u
#define CPU_P_V 0x40u
#define CPU_P_N 0x80u

static inline void cpu_p_to_mirrors(CpuState *cpu) {
    cpu->m_flag  = (cpu->P & CPU_P_M) ? 1 : 0;
    cpu->x_flag  = (cpu->P & CPU_P_X) ? 1 : 0;
    cpu->_flag_C = (cpu->P & CPU_P_C) ? 1 : 0;
    cpu->_flag_Z = (cpu->P & CPU_P_Z) ? 1 : 0;
    cpu->_flag_I = (cpu->P & CPU_P_I) ? 1 : 0;
    cpu->_flag_D = (cpu->P & CPU_P_D) ? 1 : 0;
    cpu->_flag_V = (cpu->P & CPU_P_V) ? 1 : 0;
    cpu->_flag_N = (cpu->P & CPU_P_N) ? 1 : 0;
}
static inline void cpu_mirrors_to_p(CpuState *cpu) {
    cpu->P = (uint8)(
        (cpu->m_flag  ? CPU_P_M : 0) |
        (cpu->x_flag  ? CPU_P_X : 0) |
        (cpu->_flag_C ? CPU_P_C : 0) |
        (cpu->_flag_Z ? CPU_P_Z : 0) |
        (cpu->_flag_I ? CPU_P_I : 0) |
        (cpu->_flag_D ? CPU_P_D : 0) |
        (cpu->_flag_V ? CPU_P_V : 0) |
        (cpu->_flag_N ? CPU_P_N : 0)
    );
}
static inline uint8 cpu_read_b(const CpuState *cpu) {
    return (uint8)((cpu->A >> 8) & 0xFF);
}

/* WRAM bank routing — same map as cpu_state.c::cpu_ram_offset. */
static int cpu_ram_offset(uint8 bank, uint16 addr) {
    if (bank == 0x7E) return (int)addr;
    if (bank == 0x7F) return 0x10000 + (int)addr;
    if (addr < 0x2000 && (bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF)))
        return (int)addr;
    return -1;
}
static uint8 cpu_read8(CpuState *cpu, uint8 bank, uint16 addr) {
    int off = cpu_ram_offset(bank, addr);
    return (off >= 0) ? cpu->ram[off] : 0xFF;
}
static uint16 cpu_read16(CpuState *cpu, uint8 bank, uint16 addr) {
    int off = cpu_ram_offset(bank, addr);
    if (off >= 0 && off + 1 < 0x20000)
        return (uint16)cpu->ram[off] | ((uint16)cpu->ram[off+1] << 8);
    return 0xFFFF;
}
static void cpu_write8(CpuState *cpu, uint8 bank, uint16 addr, uint8 v) {
    int off = cpu_ram_offset(bank, addr);
    if (off >= 0) cpu->ram[off] = v;
}
static void cpu_write16(CpuState *cpu, uint8 bank, uint16 addr, uint16 v) {
    int off = cpu_ram_offset(bank, addr);
    if (off >= 0 && off + 1 < 0x20000) {
        cpu->ram[off]   = (uint8)(v & 0xFF);
        cpu->ram[off+1] = (uint8)(v >> 8);
    }
}

/* No-op stubs for tracing helpers some emits reference. */
static void cpu_trace_event(CpuState *cpu, uint32 a, uint8 b, uint8 c, uint16 d) {
    (void)cpu; (void)a; (void)b; (void)c; (void)d;
}
static void cpu_trace_px_record(CpuState *cpu, uint32 a, uint8 b, uint8 c, uint8 d) {
    (void)cpu; (void)a; (void)b; (void)c; (void)d;
}
"""
