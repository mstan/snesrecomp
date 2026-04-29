#pragma once

/*
 * snesrecomp v2 runtime CpuState.
 *
 * The single mutable container for 65816 register + flag state at
 * runtime. Every v2-recompiled function takes `CpuState *cpu` as its
 * sole parameter and mutates `cpu->A`, `cpu->X`, etc., directly. No
 * return values, no per-function locals masquerading as registers.
 *
 * REPLACES v1's per-function locals + decode-time M/X metadata fiction
 * + struct-packed return types (RetAY, RetY, PairU16, HdmaPtrs, etc.).
 *
 * v2 hand-written runtime bodies (NMI/IRQ entry, PPU/DMA orchestration,
 * etc.) keep working because `cpu->ram` aliases `g_ram` and existing
 * `g_ram[addr]` reads/writes from those bodies see the same bytes the
 * recompiled code sees.
 *
 * `m_flag` / `x_flag` / `emulation` are mirrors of P bits 5, 4, and the
 * E flag respectively. They're carried as their own slots so codegen
 * doesn't have to re-decode P every memory access; `RepFlags` /
 * `SepFlags` keep them in sync with `P` on every update.
 */

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Register / flag state ─────────────────────────────────────────────── */

typedef struct CpuState {
    /* Accumulator and index registers. Stored as 16-bit always; the
     * M / X flags govern semantic width when codegen reads/writes them.
     * (M=1 -> only the low byte of A is "live"; the high byte lives in
     * the B field below.) */
    uint16 A;
    uint8  B;       /* B half of the 16-bit accumulator pair (XBA-swappable). */
    uint16 X;
    uint16 Y;

    /* Stack and direct-page pointers, bank registers. */
    uint16 S;
    uint16 D;
    uint8  DB;
    uint8  PB;

    /* Status register P (full byte). Individual bit mirrors below for
     * codegen efficiency — they MUST be kept in sync via the helpers
     * declared below (or RepFlags / SepFlags / SetFlag IR ops). */
    uint8  P;

    /* Mirrors of P bits 5 and 4 plus the E flag. 1 = 8-bit width. */
    uint8  m_flag;
    uint8  x_flag;
    uint8  emulation;

    /* Per-flag bit mirrors. v2 codegen reads/writes `cpu->_flag_C`
     * etc. directly (rather than masking P each access). They MUST be
     * kept in sync with `P` via cpu_p_to_mirrors / cpu_mirrors_to_p
     * on every operation that updates P (REP/SEP/PLP/RTI). */
    uint8  _flag_N;
    uint8  _flag_V;
    uint8  _flag_Z;
    uint8  _flag_C;
    uint8  _flag_I;
    uint8  _flag_D;

    /* RAM. Points at the runtime's `g_ram[]` 128KB region — same bytes
     * the existing hand-written runtime reads/writes. v2 codegen will
     * issue cpu_readN / cpu_writeN against this pointer so DB / D / S
     * / PB-relative addressing all resolve through the cpu_ helpers. */
    uint8 *ram;
} CpuState;

/* P-bit positions (matches 65816 hardware). */
#define CPU_P_C  0x01u  /* Carry */
#define CPU_P_Z  0x02u  /* Zero */
#define CPU_P_I  0x04u  /* IRQ disable */
#define CPU_P_D  0x08u  /* Decimal */
#define CPU_P_X  0x10u  /* Index width (1=8-bit) */
#define CPU_P_M  0x20u  /* Memory/A width (1=8-bit) */
#define CPU_P_V  0x40u  /* Overflow */
#define CPU_P_N  0x80u  /* Negative */

/* Sync P <-> mirrors. Codegen calls these whenever P is touched in a
 * way that updates the bit mirrors (REP, SEP, PLP, RTI). */
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

/* ── Memory access ──────────────────────────────────────────────────────── */

/*
 * Memory helpers map a 24-bit logical address (bank << 16 | abs) onto
 * the runtime's flat `g_ram[0x20000]` according to the existing
 * snesrecomp memory map (see common_rtl.h). They do NOT perform any
 * banking arithmetic of their own beyond what the existing runtime
 * already does — they're a thin shim so the v2 codegen can speak in
 * terms of (bank, abs) without re-implementing the map.
 *
 * Width: 1 byte or 2 bytes (LE).
 *
 * The DB / D / S / PB-relative resolution lives in higher-level
 * helpers added in Phase 5/6 alongside the codegen — for Phase 4 we
 * ship just the raw byte/word read/write primitives.
 */

uint8  cpu_read8 (CpuState *cpu, uint8 bank, uint16 addr);
uint16 cpu_read16(CpuState *cpu, uint8 bank, uint16 addr);
void   cpu_write8 (CpuState *cpu, uint8 bank, uint16 addr, uint8  v);
void   cpu_write16(CpuState *cpu, uint8 bank, uint16 addr, uint16 v);

/* ── Initialisation ─────────────────────────────────────────────────────── */

/* Initialise `cpu` to a 65816 reset state: emulation=1, P=0x34
 * (M=X=I=1, others clear), S=0x01FF, D=0, DB=PB=0, A/B/X/Y zero.
 * Caller supplies the ram pointer (typically &g_ram[0]). */
void cpu_state_init(CpuState *cpu, uint8 *ram);

/* The singleton runtime CpuState. Defined alongside g_ram in
 * common_rtl.c. v2-recompiled code passes &g_cpu when it doesn't
 * thread `cpu` explicitly. */
extern CpuState g_cpu;

#ifdef __cplusplus
}
#endif
