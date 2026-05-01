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
     * In M=1 mode only the low byte is semantically live, but the high
     * byte is preserved verbatim across 8-bit ops — it's still part of
     * the 16-bit accumulator and surfaces under XBA / TDC / PHA-16.
     *
     * NOTE: there is no separate `B` field. The byte the 65816 calls B
     * is by definition `(A >> 8) & 0xFF`. A previous shadow `B` field
     * went stale on every 16-bit LDA and produced silent XBA bugs
     * (Layer-3 stripe corruption — see TROUBLESHOOTING.md). Reads of
     * "B" route through the cpu_read_b() helper below or `(A >> 8)`
     * inline; nothing ever writes B as separate state. */
    uint16 A;
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

/* Read the B byte of the 16-bit accumulator. Always equals the high
 * byte of A; provided as a helper so call sites read intent rather
 * than reaching into A bits directly. There is no `cpu_write_b` — the
 * way to "write B" on real hardware is XBA (or TCD/TDC), and those
 * route through full-A arithmetic. */
static inline uint8 cpu_read_b(const CpuState *cpu) {
    return (uint8)((cpu->A >> 8) & 0xFF);
}

/* ── Typed register access ────────────────────────────────────────────────
 *
 * The 65816 accumulator/index registers carry semantic width via M/X
 * flags. The hardware contracts:
 *   - A (m=1):  ops touch only A.low; A.high (= B) is preserved.
 *   - A (m=0):  ops touch full 16 bits.
 *   - X/Y (x=1): ops touch only the low byte; the high byte is FORCED
 *                to 0 on write (hw contract — distinct from A).
 *   - X/Y (x=0): full 16-bit.
 *
 * Use the typed helpers below at every codegen site that reads or
 * writes A/X/Y. The function name encodes the width explicitly:
 *
 *   cpu_read_a8 / a16 / x8 / x16 / y8 / y16     (bare-width readers)
 *   cpu_write_a8 / a16 / x8 / x16 / y8 / y16    (bare-width writers)
 *   cpu_read_a_m  / x_x  / y_x                   (M/X-flag dispatching reads)
 *   cpu_write_a_m / x_x  / y_x                   (M/X-flag dispatching writes)
 *
 * Why typed helpers instead of raw `cpu->A`:
 *   1. The READ TYPE forces the caller to think about width. A bare
 *      `cpu->A` returns 16 bits even in M=1 contexts; the caller can
 *      forget to mask, and the bug only surfaces when the high byte
 *      happens to be non-zero (the SMW XBA stale-shadow class).
 *   2. The WRITE semantics differ between A (preserve high) and X/Y
 *      (zero high). Encoding it in the helper name removes the foot-gun
 *      of a contributor copy-pasting the wrong shape.
 *   3. M/X-flag dispatch lives in ONE place (the helper) rather than
 *      inline at every emit site. If a future hardware nuance is
 *      discovered, it lands in the helper and every site picks it up.
 */

/* ── A-register typed access ── */

static inline uint8  cpu_read_a8(const CpuState *cpu) {
    return (uint8)(cpu->A & 0xFF);
}
static inline uint16 cpu_read_a16(const CpuState *cpu) {
    return cpu->A;
}
/* M-flag-driven read. Returns A.low zero-extended in m=1, full A in m=0.
 * Matches what `LDA` would observe when reading the accumulator at the
 * current width. */
static inline uint16 cpu_read_a_m(const CpuState *cpu) {
    return cpu->m_flag ? (uint16)cpu_read_a8(cpu) : cpu_read_a16(cpu);
}

/* 8-bit A write — preserve high byte (= B). 65816 hw contract: in M=1
 * mode, ops on A leave the high half untouched (XBA / TDC observe the
 * preserved value). Distinct from cpu_write_x8 which ZEROS the high. */
static inline void cpu_write_a8(CpuState *cpu, uint8 v) {
    cpu->A = (uint16)((cpu->A & 0xFF00) | (uint16)v);
}
static inline void cpu_write_a16(CpuState *cpu, uint16 v) {
    cpu->A = v;
}
/* M-flag-driven write. 8-bit semantics in m=1 (preserve high), full
 * 16-bit in m=0. Caller passes a 16-bit value; we mask in m=1. */
static inline void cpu_write_a_m(CpuState *cpu, uint16 v) {
    if (cpu->m_flag) cpu_write_a8(cpu, (uint8)(v & 0xFF));
    else             cpu_write_a16(cpu, v);
}

/* ── X-register typed access ── */

static inline uint8  cpu_read_x8(const CpuState *cpu) {
    return (uint8)(cpu->X & 0xFF);
}
static inline uint16 cpu_read_x16(const CpuState *cpu) {
    return cpu->X;
}
static inline uint16 cpu_read_x_x(const CpuState *cpu) {
    return cpu->x_flag ? (uint16)cpu_read_x8(cpu) : cpu_read_x16(cpu);
}

/* 8-bit X write — ZEROS high byte (65816 hw contract for x=1). This is
 * the critical difference vs cpu_write_a8 (which preserves high). The
 * historical "8-bit X/Y zero-extend" bug class (snesrecomp 6o, b39e99b)
 * happened because emitters treated X/Y like A and let stale high bytes
 * leak through indexed reads. */
static inline void cpu_write_x8(CpuState *cpu, uint8 v) {
    cpu->X = (uint16)v;
}
static inline void cpu_write_x16(CpuState *cpu, uint16 v) {
    cpu->X = v;
}
static inline void cpu_write_x_x(CpuState *cpu, uint16 v) {
    if (cpu->x_flag) cpu_write_x8(cpu, (uint8)(v & 0xFF));
    else             cpu_write_x16(cpu, v);
}

/* ── Y-register typed access (mirrors X) ── */

static inline uint8  cpu_read_y8(const CpuState *cpu) {
    return (uint8)(cpu->Y & 0xFF);
}
static inline uint16 cpu_read_y16(const CpuState *cpu) {
    return cpu->Y;
}
static inline uint16 cpu_read_y_x(const CpuState *cpu) {
    return cpu->x_flag ? (uint16)cpu_read_y8(cpu) : cpu_read_y16(cpu);
}
static inline void cpu_write_y8(CpuState *cpu, uint8 v) {
    cpu->Y = (uint16)v;  /* x=1 zeros high */
}
static inline void cpu_write_y16(CpuState *cpu, uint16 v) {
    cpu->Y = v;
}
static inline void cpu_write_y_x(CpuState *cpu, uint16 v) {
    if (cpu->x_flag) cpu_write_y8(cpu, (uint8)(v & 0xFF));
    else             cpu_write_y16(cpu, v);
}

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

/* Diagnostic — generated functions can call this to log entry. */
void cpu_dbg_funcname(const char *name);

#ifdef __cplusplus
}
#endif
