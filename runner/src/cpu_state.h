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

    /* Option-1 cpu->S return-frame ABI (see IMPROVEMENTS.md). NON-ZERO
     * when the current function was entered via a direct generated JSR/JSL
     * C call (a paired host-C caller exists and pushed a matching return
     * frame on cpu->S) — and the value IS the pushed frame size in bytes:
     * 2 for a JSR-paired caller, 3 for a JSL-paired caller. 0 when entered
     * via cpu_dispatch_pc_from / PEI-RTL trampoline / interrupt / dynarec
     * (no proven paired host caller). Each function captures it into a
     * local `_hrv` at entry; the caller sets it right before every invoke
     * (direct JSR call -> 2, JSL -> 3; tail JMP/JML -> propagate the
     * caller's _hrv; dispatch -> 0). RTS/RTL may return RECOMP_RETURN_NORMAL
     * only when _hrv!=0 AND the stack was balanced at entry (cpu->S ==
     * _entry_s); otherwise it dispatches on the popped PC24. The frame-size
     * payload exists for cpu_unresolved_abandon_balanced(): at an
     * unresolved control transfer there is no RTS/RTL op to take a static
     * frame size from, so the abandon path pops the caller's frame by the
     * size the caller actually pushed. The legacy boolean value 1 is
     * INVALID — every setter must write 0/2/3. */
    uint8  host_return_valid;

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

    /* Axis-2 cycle accounting (SNES_ACCURACY_BURNDOWN.md). Cumulative 65816
     * CPU (bus) cycles, charged by the v2 emitter as a per-block integer
     * constant (recompiler/snes_cycles.py is the authority; the interp816
     * reference + bsnes hook validate it). One add per block — near-free, the
     * "cheap inline accounting" of the anchors-not-lockstep model. Nothing
     * reads it yet, so adding it is behavior-identical; once validated against
     * bsnes it can drive APU pacing (Axis 5 off-cue) and H/V derivation. */
    uint64_t cycles;

    /* Axis-5 off-cue: cumulative SNES MASTER clocks (21.47727 MHz), the
     * region-weighted companion to `cycles`. Each block charges its CPU-cycle
     * contribution multiplied by the code region's access speed (6 fast /
     * 8 slow / 12 xslow; memsel-aware for $80-FF), per recompiler/snes_cycles.py
     * region_speed(). Unlike `cycles` (bus cycles, validated vs bsnes), this is
     * elapsed *time* in master clocks — the correct unit to pace the SPC700,
     * which runs in its own clock domain. rtl_accumulate_apu_catchup() converts
     * the per-touch delta to SPC cycles (x 1.024MHz / 21.477MHz), REPLACING the
     * old +256-main-cycles-per-APU-touch synthetic estimate (the off-cue root).
     * Charged in the same one-add-per-block spot as `cycles` (near-free). */
    uint64_t master_cycles;
} CpuState;

/* NB: NLR pending-skip state is intentionally NOT a CpuState field.
 * It's declared as a function-local `RecompReturn _pending_skip` at
 * the top of every emitted v2 function — see emit_function.py. NLR
 * skip is C control-flow state, not 65816 hardware state, and a
 * prior `cpu->pending_skip` field on CpuState produced layout/
 * aliasing weirdness around the cpu pointer that masked the bug
 * the dynamic auditor was supposed to characterize. The local
 * variable is invisible to unrelated runtime helpers, can be kept
 * in a register by the optimizer, and matches the actual lifetime
 * of the signal (only meaningful within the currently executing
 * generated function).
 */

/* Read the B byte of the 16-bit accumulator. Always equals the high
 * byte of A; provided as a helper so call sites read intent rather
 * than reaching into A bits directly. There is no `cpu_write_b` — the
 * way to "write B" on real hardware is XBA (or TCD/TDC), and those
 * route through full-A arithmetic. */
static inline uint8 cpu_read_b(const CpuState *cpu) {
    return (uint8)((cpu->A >> 8) & 0xFF);
}

/* ── Non-local return signaling ────────────────────────────────────────
 *
 * Some 65816 functions implement "return-to-grandparent" via the
 * stack-discard idiom:
 *     PLA          ; pop own JSR return PC low
 *     PLA          ; pop own JSR return PC high
 *     ...          ; (or 3 PLAs for JSL/RTL)
 *     RTS          ; now pops grandparent's return PC
 *
 * In v2 codegen, JSR/JSL/RTS/RTL don't push or pop return-PC bytes on
 * the simulated SNES stack — those are tracked purely via C call
 * frames. So translating PLA literally as `cpu->S += 1; A = ram[S]`
 * would consume bytes that belong to AN ANCESTOR'S stack frame and
 * leave the SNES S register drifted (root cause of "DB=$C0 at
 * ProcessGameMode entry" 2026-05-02).
 *
 * Instead, the v2 ABI returns a small enum: NORMAL means "RTS to
 * immediate caller"; SKIP_N means "skip N additional levels of C
 * return" (i.e., the asm RTS would have unwound past N JSR frames).
 *
 * Callsite contract (emitted by codegen):
 *     RecompReturn _r = Callee(cpu);
 *     if (_r != RECOMP_RETURN_NORMAL) {
 *         return (RecompReturn)((int)_r - 1);
 *     }
 *
 * SKIP_N is set by NLR-pattern blocks via `cpu->pending_skip` (below);
 * the next Return op consumes it. */
typedef enum RecompReturn {
    RECOMP_RETURN_NORMAL = 0,
    RECOMP_RETURN_SKIP_1 = 1,
    RECOMP_RETURN_SKIP_2 = 2,
    RECOMP_RETURN_SKIP_3 = 3,
} RecompReturn;

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

/* ── Interrupt-frame ABI (Option-1 cpu->S model) ────────────────────────── */

/* Model the 65816 hardware interrupt-entry push so a handler invoked via a
 * plain host-C call (the game's rtl I_NMI / I_IRQ wrappers) leaves cpu->S in
 * the state the handler's RTI expects. Hardware pushes, in order (S
 * decreasing): native = PB, PCH, PCL, P (4 bytes); emulation = PCH, PCL, P
 * (3 bytes). The handler's RTI (codegen _emit_return op.interrupt) pops in
 * the mirror order: pull P (top), discard PC (2), native discard PB (1).
 * PC/PB are discarded on RTI (host-C return carries control), so only P must
 * be live — it is restored to the interrupted code. MUST be paired with that
 * RTI pop; without this push the RTI over-pops cpu->S. */
static inline void cpu_push_interrupt_frame(CpuState *cpu) {
    /* v2 CpuState has no runtime PC (control flow is host-C call structure),
     * so the PC bytes are pushed as zero placeholders — RTI discards them.
     * Only P is live; cpu_mirrors_to_p syncs it from the flag mirrors first. */
    cpu_mirrors_to_p(cpu);
    if (!cpu->emulation) {
        cpu_write8(cpu, 0x00, cpu->S, cpu->PB); cpu->S = (uint16)(cpu->S - 1);
    }
    cpu_write8(cpu, 0x00, cpu->S, 0x00); cpu->S = (uint16)(cpu->S - 1);  /* PCH */
    cpu_write8(cpu, 0x00, cpu->S, 0x00); cpu->S = (uint16)(cpu->S - 1);  /* PCL */
    cpu_write8(cpu, 0x00, cpu->S, cpu->P); cpu->S = (uint16)(cpu->S - 1);
}

/* Model a 65816 hardware JSR's 2-byte return-frame push. Used by game glue
 * that bare-calls a recomp entry point whose body TAIL-dispatches to handlers
 * (host_return_valid=0) — those handlers' terminal RTS pops the *caller's*
 * JSR frame (the hardware main loop did `JSR ProcessGameMode`). When the glue
 * replaces that main loop with a plain host-C call it must push the same
 * 2-byte frame, else the dispatched handler's trampoline-RTS over-pops cpu->S
 * by 2 on every call (SMW's per-frame GameMode dispatch leaked exactly this).
 * PC bytes are placeholders — the trampoline-RTS dispatch-misses on them and
 * miss-restores S to entry_s + 2. Symmetric to cpu_push_interrupt_frame. */
static inline void cpu_push_jsr_return_frame(CpuState *cpu) {
    cpu_write8(cpu, 0x00, cpu->S, 0x00); cpu->S = (uint16)(cpu->S - 1);  /* PCH */
    cpu_write8(cpu, 0x00, cpu->S, 0x00); cpu->S = (uint16)(cpu->S - 1);  /* PCL */
    cpu->host_return_valid = 2;  /* JSR-paired: 2-byte frame */
}

/* Model a 65816 hardware JSL's 3-byte return-frame push for host glue
 * that calls a long subroutine alias directly. The sentinel address is
 * only observed on a trampoline miss; a balanced RTL host-returns before
 * dispatching on it. */
static inline void cpu_push_jsl_return_frame(CpuState *cpu) {
    cpu_write8(cpu, 0x00, cpu->S, 0xFF); cpu->S = (uint16)(cpu->S - 1);  /* PB */
    cpu_write8(cpu, 0x00, cpu->S, 0xFF); cpu->S = (uint16)(cpu->S - 1);  /* PCH */
    cpu_write8(cpu, 0x00, cpu->S, 0xFF); cpu->S = (uint16)(cpu->S - 1);  /* PCL */
    cpu->host_return_valid = 3;  /* JSL-paired: 3-byte frame */
}

/* ── Initialisation ─────────────────────────────────────────────────────── */

/* Initialise `cpu` to a 65816 reset state: emulation=1, P=0x34
 * (M=X=I=1, others clear), S=0x01FF, D=0, DB=PB=0, A/B/X/Y zero.
 * Caller supplies the ram pointer (typically &g_ram[0]). */
void cpu_state_init(CpuState *cpu, uint8 *ram);

/* The singleton runtime CpuState. Defined alongside g_ram in
 * common_rtl.c. v2-recompiled code passes &g_cpu when it doesn't
 * thread `cpu` explicitly. */
extern CpuState g_cpu;

/* Live $420D FastROM (MEMSEL) bit, tracked in common_rtl.c WriteReg. Generated
 * blocks in the $80-$FF WS2 mirror banks reference it to weight their Axis-5
 * master-cycle charge (6 fast clocks/access when set, 8 slow when clear); it's
 * declared here so every generated translation unit (which always includes
 * cpu_state.h) can see it. SlowROM games never set it and emit no reference. */
extern uint8_t g_memsel;

/* Diagnostic — generated functions can call this to log entry. */
void cpu_dbg_funcname(const char *name);

/* ── PEI-trampoline dispatch (2026-05-24, narrow detector) ─────────────
 *
 * Codegen emits a trampoline-aware RTS/RTL ONLY for functions flagged
 * by the PEI/PEA/PER detector in emit_function.py. On those functions,
 * when cpu->S != _entry_s at the Return site, _emit_return pops the
 * topmost frame (2 bytes for RTS, 3 for RTL), computes (PB:PC+1), and
 * tail-calls cpu_dispatch_pc(cpu, pc24). The helper binary-searches
 * g_dispatch_table for an entry matching `pc24` and invokes the variant
 * fnptr for the runtime (m,x) flags. If pc24 isn't a known function
 * entry (typically a mid-function PC reached at the bottom of a
 * trampoline chain), the helper returns RECOMP_RETURN_NORMAL and the
 * host C call stack unwinds.
 *
 * The dispatch table is emitted by v2_regen.py at the end of each
 * regen run into `<prefix>_dispatch_v2.c`. Entries are sorted by
 * pc24 for binary search. Each row holds up to 4 fnptrs (one per
 * (m,x) variant). */
typedef struct DispatchEntry {
    uint32 pc24;
    /* Indexed by ((m_flag & 1) << 1) | (x_flag & 1):
     *   0 = M0X0, 1 = M0X1, 2 = M1X0, 3 = M1X1.
     * NULL = variant not emitted; cpu_dispatch_pc returns NORMAL. */
    RecompReturn (*variant[4])(CpuState *);
    /* Bytes embedded after a JSR/JSL and consumed by this callee.  The
     * interpreter bridge uses this to resume a bounced call at the same
     * continuation the decoder selected for compiled callers. */
    uint8 inline_arg_bytes;
} DispatchEntry;

extern const DispatchEntry g_dispatch_table[];
extern const unsigned       g_dispatch_table_count;

uint8 cpu_dispatch_inline_arg_bytes(uint32 pc24);

/* Dispatch on a popped trampoline target.
 * `entry_s_for_miss_restore`: on lookup MISS, cpu->S is set to this
 * value before returning RECOMP_RETURN_NORMAL. This prevents the
 * trampoline pop's bytes-consumed effect from leaking cpu->S drift up
 * to the caller. On lookup HIT, the dispatched function runs and its
 * own cpu->S manipulations are preserved (no restore). The trampoline-
 * emit caller in codegen._emit_return passes its function's _entry_s
 * as the restore value, matching what real hw would leave cpu->S at
 * once the trampoline chain bottomed out at the caller's continuation.
 */
RecompReturn cpu_dispatch_pc(CpuState *cpu, uint32 pc24,
                              uint16 entry_s_for_miss_restore);
RecompReturn cpu_dispatch_pc_from(CpuState *cpu, uint32 pc24,
                                  uint16 entry_s_for_miss_restore,
                                  uint32 source_pc24);

/* Runtime-pointer JSR (abs,X) call: paired host-call dispatch of a WRAM
 * handler pointer the static pass cannot enumerate (SM enemy/PLM/eproj AI
 * instruction-list interpreters). Pushes the 2-byte JSR return frame,
 * dispatches the live (m,x) variant (AOT body host-returns through the
 * frame; miss -> interpreter tier via interp_tier_run_call), and leaves the
 * stack balanced so the caller falls through to the post-JSR block. Logged
 * in g_dispatch_log. Emitted by codegen _emit_runtime_dispatch. */
RecompReturn cpu_dispatch_call_pc(CpuState *cpu, uint32 pc24,
                                  uint32 source_pc24);
/* Paired runtime-pointer call whose 2/3-byte hardware return frame is already
 * on cpu->S (e.g. PHK; PEA <ret-1>; JML [ptr]). Runs an AOT target when
 * available, otherwise interprets only the callee until it consumes that
 * frame. The generated caller then resumes its compiled continuation. */
RecompReturn cpu_dispatch_call_pc_pushed(CpuState *cpu, uint32 pc24,
                                         uint32 source_pc24,
                                         uint8 frame_size,
                                         uint32 *return_pc24);

/* Paired-call dispatch for the interpreter bridge AOT-bounce (cpu_state.c): the
 * interp already pushed the frame_size-byte return frame; run the target's live
 * (m,x) variant with hrv=frame_size so it HOST-RETURNS to the bridge instead of
 * re-dispatching on the popped return address (which over-pops when that address
 * is a registered function entry). Returns the callee's value. */
RecompReturn cpu_dispatch_pc_paired(CpuState *cpu, uint32 pc24, uint8 frame_size);

/* Read-only dispatch-table probe (task #7 RTS-decision trace). */
int cpu_dispatch_has_entry(CpuState *cpu, uint32 pc24);

/* Balanced abandon of the current function invocation at an UNRESOLVED
 * control-transfer site (unresolved indirect dispatch / OOB index,
 * cross-ROM-bank stub body, unresolvable cross-bank goto). Sets
 * cpu->S = entry_s + hrv — discarding anything the function pushed
 * below its entry baseline AND popping the paired host caller's return
 * frame by the size the caller actually pushed (hrv: 0 = no paired
 * caller, 2 = JSR, 3 = JSL; see host_return_valid above) — then
 * returns RECOMP_RETURN_NORMAL so the host C caller resumes as if the
 * unreached handler had returned immediately. CORRECTNESS infra
 * (always compiled, lives in common_cpu_infra.c), NOT trace infra:
 * without it every unresolved-site hit orphans the caller's frame and
 * cpu->S drifts until the stack corrupts (SM cinematic sub-dispatchers
 * leaked 4-7 B/frame -> WILD STACK crash ~f1516, 2026-06-09). The
 * skipped handler's SIDE EFFECTS are still missing — every hit remains
 * a real worklist item, accounted by an ALWAYS-ON per-site hit table
 * inside this helper (first hit per site -> one stderr line; full table
 * -> post-mortem JSON via CpuUnresolvedAbandonDumpJson) plus the richer
 * cpu_trace_dispatch_oob / cpu_trace_unresolved_stub_trap slot capture
 * when SNESRECOMP_TRACE is on. */
RecompReturn cpu_unresolved_abandon_balanced(CpuState *cpu, uint32 site_pc24,
                                             uint16 entry_s, uint8 hrv);

/* Interpreter-fallback tier (interp_bridge.c). Interpret the guest routine at
 * target_pc24 sharing this CpuState — bouncing sub-calls into compiled code —
 * and return when it RTS/RTLs past entry. See docs/MULTI_TIER.md.
 *
 * interp_tier_dispatch_balanced is the form used at unresolved tail-dispatch
 * sites that otherwise call cpu_unresolved_abandon_balanced: it UPGRADES the
 * stack-safe drop into actually running the routine. On a clean return the
 * routine's own RTS/RTL balances the stack; on a bail (step cap / wedge) it
 * falls back to cpu_unresolved_abandon_balanced(site,entry_s,hrv) so it is
 * never worse than the drop path and still records the site. */
RecompReturn interp_tier_dispatch(CpuState *cpu, uint32 target_pc24);
RecompReturn interp_tier_dispatch_balanced(CpuState *cpu, uint32 target_pc24,
                                           uint32 site_pc24, uint16 entry_s,
                                           uint8 hrv);

/* Interpreter-tier fallback for a runtime-pointer JSR (abs,X) whose loaded
 * target has no AOT body for the live (m,x). Called by cpu_dispatch_call_pc
 * AFTER it has pushed the 2-byte JSR return frame: the watermark is the
 * current S (post-push) so the target's RTS pops that frame and exits the
 * bridge; on a bail the post-call S is restored. Always balanced, always
 * NORMAL — the caller falls through to the post-JSR block. Recorded in the
 * tier-2 gap manifest (kind=dispatch). */
RecompReturn interp_tier_run_call(CpuState *cpu, uint32 target_pc24,
                                  uint32 source_pc24);
RecompReturn interp_tier_run_call_frame(CpuState *cpu, uint32 target_pc24,
                                        uint32 source_pc24,
                                        uint8 frame_size,
                                        uint32 *return_pc24);
/* Phase-4 (opt-in) bank-miss tier-down: the body emitted for a cross-ROM-bank
 * function the static pass couldn't translate (unresolved_stubs_v2.c). Runs
 * the real ROM bytes at addr_pc24 instead of the no-op trap; bail -> the same
 * cpu_unresolved_abandon_balanced drop (never worse). Records kind="bank_miss".
 * Only emitted when v2_regen is run with --tier-down-stubs. */
RecompReturn interp_tier_dispatch_bank_miss(CpuState *cpu, uint32 addr_pc24,
                                            uint16 entry_s, uint8 hrv);

/* ── Fiber-free LLE yield unwind (interp_bridge.c; docs/LLE_SCHEDULER.md) ──
 * Under the interpreted cooperative scheduler (interp_bridge_run_scheduler),
 * task bodies bounce to compiled code, but the guest yield primitives are
 * coroutine switches that never return to their caller — a compiled body
 * reaching one cannot host-return normally. The game's yield hle_func stubs
 * call interp_bridge_lle_yield_unwind(resume_pc24) instead: it arms a pending
 * unwind and returns a huge RecompReturn sentinel that every emitted callsite
 * propagates (`return _r - 1`), unwinding the host C stack from arbitrary
 * depth back to the scheduler frame's bounce site. That site consumes the
 * request and CONTINUES INTERPRETING at resume_pc24 — the primitive's REAL
 * ROM entry, with cpu exactly as the compiled callsite left it (JSR frame
 * already pushed for JSR-reached primitives). The interpreter then performs
 * the actual coroutine switch (state/countdown -> $30/$31,X, TSC -> saved-S
 * slot, re-enter the slot walk) byte-exact by construction — no hand-written
 * effect duplication, no fibers.
 *
 * interp_bridge_in_lle_scheduler() tells a stub whether a scheduler frame is
 * beneath it on the host stack (0 => use the fiber/HLE path). The sentinel
 * base is far above any genuine SKIP_N, so per-level decrements can never
 * decay it into a value mistaken for a local NLR before the (bounded ≪ 2^30)
 * host stack unwinds. */
#define RECOMP_RETURN_LLE_UNWIND_BASE 0x40000000
int interp_bridge_in_lle_scheduler(void);
RecompReturn interp_bridge_lle_yield_unwind(CpuState *cpu, uint32 resume_pc24);
uint32 interp_bridge_lle_resume_pc(void);

/* Focused OAM-overflow observability recorders (debug_server.c).
 * dbg_rts_trace is emitted by the RTS/RTL lowering; dbg_oam_block_trace
 * is called from cpu_trace_block. Both are PC-range-filtered and frozen
 * by the [oamdrop] g_boundary_frozen tripwire. */
void dbg_rts_trace(CpuState *cpu, uint32 src_pc, uint16 entry_s,
                   uint16 ret_s, uint32 popped_pc, uint8 hrv);
void dbg_oam_block_trace(CpuState *cpu, uint32 pc24);

#ifdef __cplusplus
}
#endif
