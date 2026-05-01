"""snesrecomp.recompiler.v2.codegen

Emit C code from v2 IR. Generated functions take a single
`CpuState *cpu` parameter and mutate `cpu->A`, `cpu->X`, etc., directly
— no return values, no per-function locals masquerading as registers.

Replaces v1 EmitCtx C-expression-string-based codegen
(recomp.py:2829-6200) including the heuristic phi machinery
(_branch_states, _label_a/b/x/y, _emit_backedge_phi, _emit_branch,
_ensure_mutable_x). v2 codegen has no per-function abstract register
state at emit time — register reads/writes are explicit memory loads
and stores against the CpuState struct.

Every IR Value produced by an IR op becomes a fresh C local. A
`Value(vid=N)` lowers to `_v<N>`. Width is inferred per op (the IR
op type carries the width).

Public API:
    emit_block(block: IRBlock, *, indent: str = "  ") -> List[str]

Phase 5 of plan parsed-skipping-rainbow.md. Phase 6 will wire this
into a per-function emit driver (replacing the v1 emit_function) and
run the full SMW regen against it.
"""

import sys
import pathlib

_THIS_DIR = pathlib.Path(__file__).resolve().parent
_RECOMPILER_DIR = _THIS_DIR.parent
for p in (str(_THIS_DIR), str(_RECOMPILER_DIR)):
    if p not in sys.path:
        sys.path.insert(0, p)

from typing import Dict, List  # noqa: E402

# Resolver: 24-bit address (bank << 16 | pc) -> friendly C function name.
# Populated by emit_bank before each bank emit (a process-wide map of every
# `func`/`name` declaration across all banks loaded so far). When a Call op
# resolves to one of these addresses, codegen emits the friendly name; else
# it falls back to the synthetic `bank_BB_AAAA` form.
_NAME_RESOLVER: Dict[int, str] = {}

# Set of (24-bit Call target, entry_m, entry_x) tuples for EVERY Call
# emitted, regardless of whether the friendly name resolved. v2_regen
# diffs this against the set of (addr, m, x) variants actually emitted
# and adds missing entries to cover any unmet demand.
#
# Why track ALL targets, not just unresolved ones: cfg-named targets
# (e.g. UpdateEntirePalette) might only have an M1X1 entry in cfg, but
# get called from M0X0 callers. The Call site needs UpdateEntirePalette
# _M0X0 to exist; tracking the (target, m, x) tuple lets v2_regen
# discover the unmet variant and clone the cfg entry at the new (m, x).
#
# Per-(m, x) tracking: a 65816 function decoded with M=1 X=1 is a
# different instruction stream than M=1 X=0 because LDX #imm consumes
# 2 vs 3 bytes (and LDA/LDY immediates similarly with M). So a single
# ROM function reachable from contexts with different (m, x) must emit
# multiple C bodies.
_UNRESOLVED_CALL_TARGETS: set = set()


def _variant_suffix(m: int, x: int) -> str:
    """Return the `_M{m}X{x}` suffix used for per-variant function names.

    Centralised so emit_function, _emit_call, and the cross-tool
    sync_funcs_h regen all agree on the mangling. Suffix is universal
    in v2 — every gen function name carries it, every call site
    appends it. Hand-written entry-point shims (e.g. I_RESET in
    smw_rtl.c) rely on cfg-emitted aliases that drop the suffix for
    the cfg-default (m,x).
    """
    return f"_M{m & 1}X{x & 1}"


def set_name_resolver(name_map: Dict[int, str]) -> None:
    """Replace the call-target name resolver. Pass an empty dict to clear."""
    global _NAME_RESOLVER
    _NAME_RESOLVER = dict(name_map)


def take_unresolved_call_targets() -> set:
    """Return + clear the set of synthetic-name Call targets seen since
    the last call. Used by v2_regen for iterative auto-promote."""
    global _UNRESOLVED_CALL_TARGETS
    out = _UNRESOLVED_CALL_TARGETS
    _UNRESOLVED_CALL_TARGETS = set()
    return out


from v2 import widths  # noqa: E402
from v2 import emitter_helpers  # noqa: E402
from v2.ir import (  # noqa: E402
    IROp, IRBlock,
    Read, Write, ReadReg, WriteReg, ConstI,
    Alu, AluOp, Shift, ShiftOp, IncReg, IncMem,
    BitTest, BitSetMem, BitClearMem,
    SetFlag, SetNZ, RepFlags, SepFlags, XCE,
    Push, Pull, PushReg, PullReg, BlockMove,
    CondBranch, Goto, IndirectGoto, Call, Return,
    Transfer, XBA, Nop, Break, Stop, PushEffectiveAddress,
    Reg, SegRef, SegKind, Value,
)


# ── Helpers ─────────────────────────────────────────────────────────────────

def _v(value: Value) -> str:
    """Format a Value as its C local name."""
    return f"_v{value.vid}"


def _ctype(width: int) -> str:
    return "uint8" if width == 1 else "uint16"


# Reg → CpuState field expression.
#
# Reg.B is intentionally a DERIVED expression, not a struct field. The 65816
# B register is the high byte of the 16-bit accumulator and ALWAYS equals
# `(A >> 8) & 0xFF`. Earlier versions had a `cpu->B` shadow field; that
# field went stale every time a 16-bit LDA wrote `cpu->A` without syncing
# the shadow, and XBA-after-LDA-in-m=0 swapped in stale bytes (SMW
# Layer-3 stripe corruption, fixed in commit 6c04c94, then removed
# entirely in this commit).
_REG_FIELD = {
    Reg.A: "cpu->A", Reg.B: "((uint8)((cpu->A >> 8) & 0xFF))",
    Reg.X: "cpu->X", Reg.Y: "cpu->Y",
    Reg.S: "cpu->S", Reg.D: "cpu->D",
    Reg.DB: "cpu->DB", Reg.PB: "cpu->PB",
    Reg.P: "cpu->P",
    Reg.M: "cpu->m_flag", Reg.XF: "cpu->x_flag", Reg.E: "cpu->emulation",
    Reg.N: "cpu->_flag_N", Reg.V: "cpu->_flag_V",
    Reg.ZF: "cpu->_flag_Z", Reg.C: "cpu->_flag_C",
    Reg.I: "cpu->_flag_I", Reg.DF: "cpu->_flag_D",
}


def _reg(r: Reg) -> str:
    return _REG_FIELD[r]


# ── SegRef → C address expressions ──────────────────────────────────────────

def _segref_addr_expr(seg: SegRef) -> tuple:
    """Resolve a SegRef into (bank_expr, addr_expr) C strings.

    bank_expr / addr_expr reference cpu state where appropriate. The
    caller passes them to cpu_read* / cpu_write* primitives.
    """
    idx = ""
    if seg.index == Reg.X:
        idx = " + cpu->X"
    elif seg.index == Reg.Y:
        idx = " + cpu->Y"

    k = seg.kind
    if k == SegKind.DIRECT:
        return ("0x7E", f"(uint16)(cpu->D + {seg.offset:#06x}{idx})")
    if k == SegKind.ABS_BANK:
        return ("cpu->DB", f"(uint16)({seg.offset:#06x}{idx})")
    if k == SegKind.LONG:
        bank = seg.bank if seg.bank is not None else 0
        return (f"{bank:#04x}", f"(uint16)({seg.offset:#06x}{idx})")
    if k == SegKind.STACK:
        return ("0x00", f"(uint16)(cpu->S + {seg.offset:#06x})")
    if k == SegKind.DP_INDIRECT:
        # ((D + dp) word) (+ Y if indirect-Y), DB-bank.
        ptr_addr = f"(uint16)(cpu->D + {seg.offset:#06x})"
        return ("cpu->DB", f"(uint16)(cpu_read16(cpu, 0x00, {ptr_addr}){idx})")
    if k == SegKind.DP_INDIRECT_LONG:
        # ((D + dp) long) (+ Y).
        ptr_addr = f"(uint16)(cpu->D + {seg.offset:#06x})"
        bank_expr = f"cpu_read8(cpu, 0x00, (uint16)({ptr_addr} + 2))"
        addr_expr = f"(uint16)(cpu_read16(cpu, 0x00, {ptr_addr}){idx})"
        return (bank_expr, addr_expr)
    if k == SegKind.ABS_INDIRECT:
        return ("cpu->PB",
                f"cpu_read16(cpu, cpu->PB, (uint16){seg.offset:#06x})")
    if k == SegKind.ABS_INDIRECT_X:
        return ("cpu->PB",
                f"cpu_read16(cpu, cpu->PB, (uint16)({seg.offset:#06x} + cpu->X))")
    if k == SegKind.ABS_INDIRECT_LONG:
        addr = f"(uint16){seg.offset:#06x}"
        return (f"cpu_read8(cpu, 0x00, (uint16)({addr} + 2))",
                f"cpu_read16(cpu, 0x00, {addr})")
    if k == SegKind.DP_INDIRECT_X:
        ptr_addr = f"(uint16)(cpu->D + {seg.offset:#06x} + cpu->X)"
        return ("cpu->DB", f"cpu_read16(cpu, 0x00, {ptr_addr})")
    if k == SegKind.STACK_REL_INDIRECT_Y:
        ptr_addr = f"(uint16)(cpu->S + {seg.offset:#06x})"
        return ("cpu->DB",
                f"(uint16)(cpu_read16(cpu, 0x00, {ptr_addr}) + cpu->Y)")
    raise ValueError(f"unsupported SegKind {k}")


# ── Per-op handlers ─────────────────────────────────────────────────────────

def _emit_read(op: Read) -> List[str]:
    bank, addr = _segref_addr_expr(op.seg)
    return [f"{widths.ctype(op.width)} {_v(op.out)} = "
            f"{widths.read_fn(op.width)}(cpu, {bank}, {addr});"]


def _emit_write(op: Write) -> List[str]:
    bank, addr = _segref_addr_expr(op.seg)
    return [f"{widths.write_fn(op.width)}(cpu, {bank}, {addr}, {_v(op.src)});"]


def _emit_readreg(op: ReadReg) -> List[str]:
    """Emit a 16-bit read of A/X/Y/etc. For A, X, Y we route through
    cpu_read_{a,x,y}16 helpers — same value, but the helper name carries
    the hardware contract and the lint can spot bypass attempts. For
    Reg.B the _REG_FIELD mapping returns the derived `(cpu->A >> 8)`
    expression directly.

    NOTE: This always emits a 16-bit uint. Callers that want 8-bit must
    mask via widths.masked(). Width-aware callers (e.g. ALU, BIT, INC)
    already do this at op-level. The 8-bit-direct helpers
    (cpu_read_a8/x8/y8) exist for hand-written runtime code, not v2
    codegen output."""
    if op.reg == Reg.A:
        return [f"uint16 {_v(op.out)} = cpu_read_a16(cpu);"]
    if op.reg == Reg.X:
        return [f"uint16 {_v(op.out)} = cpu_read_x16(cpu);"]
    if op.reg == Reg.Y:
        return [f"uint16 {_v(op.out)} = cpu_read_y16(cpu);"]
    return [f"uint16 {_v(op.out)} = (uint16){_reg(op.reg)};"]


def _emit_writereg(op: WriteReg) -> List[str]:
    """Width-respecting write into A / X / Y, routed through the typed
    helpers in cpu_state.h. The helpers encapsulate the hardware
    contract:

    - cpu_write_a_m: in m=1, preserve A.high (= B). In m=0, full
      16-bit replace. Callers don't have to remember the asymmetry
      vs X/Y.

    - cpu_write_x_x / cpu_write_y_x: in x=1, ZERO the high byte (hw
      contract). In x=0, full 16-bit replace. The historical "8-bit
      X/Y zero-extend" bug class (snesrecomp 6o, b39e99b) was
      contributors copy-pasting the A shape onto X/Y and letting
      stale 16-bit residuals leak through indexed reads. The helper
      makes that copy-paste impossible — the hardware contract is
      part of the function name.

    Other registers (S, D, DB, PB, P) use direct field assignment;
    they have a single canonical width.
    """
    src = _v(op.src)
    if op.reg == Reg.A:
        return [f"cpu_write_a_m(cpu, (uint16)({src}));"]
    if op.reg == Reg.X:
        return [f"cpu_write_x_x(cpu, (uint16)({src}));"]
    if op.reg == Reg.Y:
        return [f"cpu_write_y_x(cpu, (uint16)({src}));"]
    return [f"{_reg(op.reg)} = {src};"]


def _emit_consti(op: ConstI) -> List[str]:
    return [f"{_ctype(op.width)} {_v(op.out)} = {op.value:#x};"]


def _emit_alu(op: Alu) -> List[str]:
    """Emit an ALU op. Internal `_t` temp is named per-output-vid (or
    per-lhs-vid for CMP which has no out) so multiple ALU ops in the
    same C function don't conflict on `_t`.

    Width contract — see `widths.py` (canonical width-literal home):
    ReadReg always emits a uint16 read of cpu->A/X/Y, so width=1 ALU
    ops MUST mask both operands via `widths.masked` before computing
    carry/borrow/sign. Otherwise the high byte (B-register for A, or
    stale hw-zero for X/Y) leaks into the result.
    """
    if op.out is not None:
        tname = f"_t{op.out.vid}"
    else:
        tname = f"_tc{op.lhs.vid}_{op.rhs.vid}"  # CMP: no out

    lines = []
    lhs_m = widths.masked(_v(op.lhs), op.width)
    rhs_m = widths.masked(_v(op.rhs), op.width)
    if op.op == AluOp.ADD:
        lines.append(
            f"uint32 {tname} = (uint32){lhs_m} + (uint32){rhs_m} + cpu->_flag_C;"
        )
        if op.out is not None:
            lines.append(f"{widths.ctype(op.width)} {_v(op.out)} = ({widths.ctype(op.width)}){tname};")
        lines.append(widths.set_carry_from_overflow(tname, op.width, "add"))
        # V flag for ADC: (lhs ^ result) & (rhs ^ result) & sign_bit
        if op.out is not None:
            lines.append(widths.set_v_adc(lhs_m, rhs_m, _v(op.out), op.width))
    elif op.op == AluOp.SUB:
        lines.append(
            f"uint32 {tname} = (uint32){lhs_m} - (uint32){rhs_m} - (1 - cpu->_flag_C);"
        )
        if op.out is not None:
            lines.append(f"{widths.ctype(op.width)} {_v(op.out)} = ({widths.ctype(op.width)}){tname};")
        lines.append(widths.set_carry_from_overflow(tname, op.width, "sub"))
        # V flag for SBC: (lhs ^ rhs) & (lhs ^ result) & sign_bit
        if op.out is not None:
            lines.append(widths.set_v_sbc(lhs_m, rhs_m, _v(op.out), op.width))
    elif op.op == AluOp.AND:
        lines.append(
            f"{widths.ctype(op.width)} {_v(op.out)} = "
            f"({widths.ctype(op.width)})({_v(op.lhs)} & {_v(op.rhs)});"
        )
    elif op.op == AluOp.OR:
        lines.append(
            f"{widths.ctype(op.width)} {_v(op.out)} = "
            f"({widths.ctype(op.width)})({_v(op.lhs)} | {_v(op.rhs)});"
        )
    elif op.op == AluOp.XOR:
        lines.append(
            f"{widths.ctype(op.width)} {_v(op.out)} = "
            f"({widths.ctype(op.width)})({_v(op.lhs)} ^ {_v(op.rhs)});"
        )
    elif op.op == AluOp.CMP:
        lines.append(
            f"uint32 {tname} = (uint32){lhs_m} - (uint32){rhs_m};"
        )
        lines.append(f"cpu->_flag_C = ({lhs_m} >= {rhs_m}) ? 1 : 0;")
        # CMP doesn't update cpu->P here either — historical
        # behavior matched _emit_shift; both now route through helpers.
        lines.extend(widths.set_nz_no_p(f"({widths.ctype(op.width)}){tname}", op.width))
        return lines

    if op.out is not None:
        # Result is already in width-typed _v(op.out), so set N/Z from
        # it. Skip cpu->P update for ALU (preserves historical
        # behavior; SEP/REP at next mode boundary will resync via
        # cpu_mirrors_to_p as fixed in 44c96a7).
        lines.extend(widths.set_nz_no_p(_v(op.out), op.width))
    return lines


def _emit_shift(op: Shift) -> List[str]:
    """Width contract — see `widths.py`. The pre-DRY emitter forgot
    the `widths.masked` step on src for several years (b39e99b/8f9369d
    fixed it reactively per op). Now uniform via helpers."""
    src_m = widths.masked(_v(op.src), op.width)
    sign = widths.sign_bit(op.width)
    out_v = _v(op.out)
    out_t = widths.ctype(op.width)
    if op.op == ShiftOp.ASL:
        return [
            f"{out_t} {out_v} = ({out_t})({src_m} << 1);",
            widths.set_carry_from_bit(src_m, sign),
        ] + widths.set_nz_no_p(out_v, op.width)
    if op.op == ShiftOp.LSR:
        return [
            f"{out_t} {out_v} = ({out_t})({src_m} >> 1);",
            widths.set_carry_from_bit(src_m, "1"),
        ] + widths.set_nz_no_p(out_v, op.width)
    if op.op == ShiftOp.ROL:
        return [
            f"{out_t} {out_v} = "
            f"({out_t})(({src_m} << 1) | cpu->_flag_C);",
            widths.set_carry_from_bit(src_m, sign),
        ] + widths.set_nz_no_p(out_v, op.width)
    if op.op == ShiftOp.ROR:
        return [
            f"{out_t} {out_v} = "
            f"({out_t})(({src_m} >> 1) | "
            f"((uint{op.width*8})cpu->_flag_C << {op.width * 8 - 1}));",
            widths.set_carry_from_bit(src_m, "1"),
        ] + widths.set_nz_no_p(out_v, op.width)
    raise ValueError(f"unhandled Shift op {op.op}")


def _emit_increg(op: IncReg) -> List[str]:
    field = _reg(op.reg)
    delta = "1" if op.delta == +1 else "-1"
    # 65816 width semantics:
    #   INC A: width follows M (0=16-bit, 1=8-bit)
    #   INX / INY / DEX / DEY: width follows X (0=16-bit, 1=8-bit)
    # A high byte is the B register; INC A in m=1 must NOT carry into B.
    # X/Y high byte is HARDWARE-ZERO in x=1 mode (SEP #$10 zeros it at
    # the flag transition; subsequent 8-bit ops can't physically write
    # to it). Old codegen preserved X/Y high across 8-bit increments,
    # which is wrong: stale 16-bit residuals leaked through. Indexed
    # addressing then read from base + (stale_high<<8 | new_low) and
    # NMI's LoadStripeImage spun for 30k+ iterations on garbage stripe
    # data. Fixed 2026-04-30.
    if op.reg == Reg.A:
        # m=1: 8-bit INC, preserve B (high byte). m=0: 16-bit INC.
        lines = [f"if (cpu->m_flag) {{",
                 f"  uint8 _lo8 = ({widths.low_byte(field)}) + ({delta});",
                 f"  {field} = {widths.preserve_high(field, '_lo8')};"]
        lines.extend(f"  {s}" for s in widths.set_nz_no_p("_lo8", 1))
        lines.append("} else {")
        lines.append(f"  {field} = (uint16)(({field}) + ({delta}));")
        lines.extend(f"  {s}" for s in widths.set_nz_no_p(field, 2))
        lines.append("}")
        return lines
    if op.reg in (Reg.X, Reg.Y):
        # x=1: 8-bit INC, ZERO high (hw contract). x=0: 16-bit INC.
        lines = [f"if (cpu->x_flag) {{",
                 f"  uint8 _lo8 = ({widths.low_byte(field)}) + ({delta});",
                 f"  {field} = {widths.zero_extend_lo('_lo8')};"
                 f"  /* x=1 zeros high byte (hw contract) */"]
        lines.extend(f"  {s}" for s in widths.set_nz_no_p("_lo8", 1))
        lines.append("} else {")
        lines.append(f"  {field} = (uint16)(({field}) + ({delta}));")
        lines.extend(f"  {s}" for s in widths.set_nz_no_p(field, 2))
        lines.append("}")
        return lines
    # Other registers (D, S) — always 16-bit native.
    return [f"{field} = ({field}) + ({delta});"] + widths.set_nz_no_p(field, 2)


def _emit_incmem(op: IncMem) -> List[str]:
    """INC/DEC memory: result = mem + delta (no carry-in); set Z/N from
    result; leave C and V untouched. 65816 hw spec for INC/DEC abs/dp.
    Distinct from ADC/SBC (Alu.ADD/SUB) which DO carry-in and update C/V."""
    bank, addr = _segref_addr_expr(op.seg)
    delta = "+1" if op.delta == +1 else "-1"
    ctype = widths.ctype(op.width)
    lines = [
        "{",
        f"  {ctype} _im = {widths.read_fn(op.width)}(cpu, {bank}, {addr});",
        f"  _im = ({ctype})(_im {delta});",
        f"  {widths.write_fn(op.width)}(cpu, {bank}, {addr}, _im);",
    ]
    lines.extend(f"  {s}" for s in widths.set_nz_no_p("_im", op.width))
    lines.append("}")
    return lines


def _emit_bittest(op: BitTest) -> List[str]:
    """BIT instruction: Z from A AND mem, N/V from mem bits.
    A is masked via cast through ctype to avoid B-register leaking.
    N/V bits are width-relative — see `widths.sign_bit`/`overflow_bit`."""
    sign = widths.sign_bit(op.width)
    overflow = widths.overflow_bit(op.width)
    ctype = widths.ctype(op.width)
    a_m = widths.masked("cpu->A", op.width)
    operand_m = widths.masked(_v(op.operand), op.width)
    return [
        "{",
        f"  {ctype} _bt = ({ctype})({a_m} & {operand_m});",
        f"  cpu->_flag_Z = (_bt == 0) ? 1 : 0;",
        f"  cpu->_flag_N = (({operand_m} & {sign}) != 0) ? 1 : 0;",
        f"  cpu->_flag_V = (({operand_m} & {overflow}) != 0) ? 1 : 0;",
        "}",
    ]


def _emit_bitsetmem(op: BitSetMem) -> List[str]:
    bank, addr = _segref_addr_expr(op.seg)
    ctype = widths.ctype(op.width)
    return [
        "{",
        f"  {ctype} _m = {widths.read_fn(op.width)}(cpu, {bank}, {addr});",
        f"  cpu->_flag_Z = ((_m & cpu->A) == 0) ? 1 : 0;",
        f"  {widths.write_fn(op.width)}(cpu, {bank}, {addr}, ({ctype})(_m | cpu->A));",
        "}",
    ]


def _emit_bitclearmem(op: BitClearMem) -> List[str]:
    bank, addr = _segref_addr_expr(op.seg)
    ctype = widths.ctype(op.width)
    return [
        "{",
        f"  {ctype} _m = {widths.read_fn(op.width)}(cpu, {bank}, {addr});",
        f"  cpu->_flag_Z = ((_m & cpu->A) == 0) ? 1 : 0;",
        f"  {widths.write_fn(op.width)}(cpu, {bank}, {addr}, ({ctype})(_m & ~cpu->A));",
        "}",
    ]


def _emit_setflag(op: SetFlag) -> List[str]:
    # Update both the per-flag mirror and the canonical cpu->P bit,
    # so subsequent PHP / direct cpu->P reads see a consistent byte.
    flag_to_p_mask = {
        Reg.C: "0x01", Reg.ZF: "0x02", Reg.I: "0x04", Reg.DF: "0x08",
        Reg.XF: "0x10", Reg.M: "0x20", Reg.V: "0x40", Reg.N: "0x80",
    }
    mask = flag_to_p_mask.get(op.flag)
    lines = [f"{_reg(op.flag)} = {op.value};"]
    if mask is not None:
        if op.value:
            lines.append(f"cpu->P = (uint8)(cpu->P | {mask});")
        else:
            lines.append(f"cpu->P = (uint8)(cpu->P & ~{mask});")
    return lines


def _emit_setnz(op) -> List[str]:
    """Update N/Z mirrors and cpu->P bits based on op.src's bits."""
    return widths.set_nz(widths.masked(_v(op.src), op.width), op.width)


def _emit_repflags(op: RepFlags) -> List[str]:
    # mirrors_to_p BEFORE modifying P (44c96a7) — see emitter_helpers.
    return ["{"] + [f"  {s}" for s in
                    emitter_helpers.modify_p_via_mirrors(op.mask, "rep")] + ["}"]


def _emit_sepflags(op: SepFlags) -> List[str]:
    return ["{"] + [f"  {s}" for s in
                    emitter_helpers.modify_p_via_mirrors(op.mask, "sep")] + ["}"]


def _emit_xce(op: XCE) -> List[str]:
    return [
        "{",
        "  uint8 _old_p = cpu->P;",
        "  uint8 _t = cpu->emulation;",
        "  cpu->emulation = cpu->_flag_C;",
        "  cpu->_flag_C = _t;",
        "  if (cpu->emulation) { cpu->m_flag = 1; cpu->x_flag = 1; cpu_mirrors_to_p(cpu); }",
        "  cpu_trace_px_record(cpu, 0, 7 /*XCE*/, _old_p, cpu->P);",
        "}",
    ]


def _emit_xba(op: XBA) -> List[str]:
    """XBA: exchange the high and low bytes of the 16-bit accumulator.
    Always 8-bit byte swap regardless of m_flag.

    Operates ENTIRELY on cpu->A — there is no separate B shadow field
    to keep in sync. The byte the 65816 calls "B" is just the high byte
    of A; it changes whenever any operation mutates A's high half (LDA
    in m=0, TCD/TDC pair manipulation, etc.). A separate `cpu->B` shadow
    invited stale-read bugs (the SMW stripe-image header parse used
    `LDA [_0],Y / XBA / AND #$3FFF / TAX`, and a stale shadow made it
    mis-derive the byte-count — visible as Layer-3 attract-demo
    scramble). The shadow has been removed; XBA must not reintroduce
    a dependency on it.

    Z/N flags are set from the new A.low byte (= old A.high), per the
    65816 manual.
    """
    return [
        "{",
        "  uint16 _old = cpu->A;",
        "  cpu->A = (uint16)(((_old & 0xFF) << 8) | ((_old >> 8) & 0xFF));",
        "  cpu->_flag_Z = ((cpu->A & 0xFF) == 0) ? 1 : 0;",
        "  cpu->_flag_N = ((cpu->A & 0x80) != 0) ? 1 : 0;",
        "}",
    ]


def _emit_pushreg(op: PushReg) -> List[str]:
    field = _reg(op.reg)
    # Push width: A/X/Y follow m/x_flag at runtime; D is always 16-bit;
    # P/DB/PB are always 1 byte.
    if op.reg == Reg.P:
        # PHP also records the P-mutation ring snapshot.
        return [
            "cpu_mirrors_to_p(cpu);",
            *emitter_helpers.push_byte(f"(uint8)({field})"),
            "cpu_trace_event(cpu, 0, CPU_TR_PHP, cpu->P, 0);",
            "cpu_trace_px_record(cpu, 0, 4 /*PHP*/, cpu->P, cpu->P);",
        ]
    if op.reg == Reg.DB:
        return emitter_helpers.push_byte(f"(uint8)({field})") + [
            "cpu_trace_event(cpu, 0, CPU_TR_PHB, cpu->DB, cpu->DB);",
        ]
    if op.reg == Reg.PB:
        # PHK pushes the program-bank K. Stale PB here is the suspected
        # root cause of bogus DB after PLB.
        return emitter_helpers.push_byte(f"(uint8)({field})") + [
            "cpu_trace_event(cpu, 0, CPU_TR_PHK, cpu->PB, cpu->PB);",
        ]
    if op.reg == Reg.D:
        return emitter_helpers.push_word(field)
    # A/X/Y: width depends on M/X flag at runtime.
    if op.reg == Reg.A:
        return ["if (cpu->m_flag) {",
                *(f"  {s}" for s in emitter_helpers.push_byte(widths.low_byte(field))),
                "} else {",
                *(f"  {s}" for s in emitter_helpers.push_word(field)),
                "}"]
    if op.reg in (Reg.X, Reg.Y):
        return ["if (cpu->x_flag) {",
                *(f"  {s}" for s in emitter_helpers.push_byte(widths.low_byte(field))),
                "} else {",
                *(f"  {s}" for s in emitter_helpers.push_word(field)),
                "}"]
    return [f"/* TODO PushReg({op.reg}) */"]


def _emit_pullreg(op: PullReg) -> List[str]:
    field = _reg(op.reg)
    if op.reg == Reg.P:
        return ["{ uint8 _old_p = cpu->P;",
                *(f"  {s}" for s in emitter_helpers.pop_byte_assign(field)),
                "  cpu_p_to_mirrors(cpu);",
                "  cpu_trace_event(cpu, 0, CPU_TR_PLP, _old_p, cpu->P);",
                "  cpu_trace_px_record(cpu, 0, 2 /*PLP*/, _old_p, cpu->P); }"]
    if op.reg == Reg.DB:
        # PLB sets N/Z from popped value.
        return ["{ uint8 _old_db = cpu->DB;",
                *(f"  {s}" for s in emitter_helpers.pop_byte_assign(field)),
                *(f"  {s}" for s in widths.set_nz(field, 1)),
                "  cpu_trace_db_change(cpu, 0, _old_db, cpu->DB, CPU_TR_PLB); }"]
    if op.reg == Reg.PB:
        # PLK doesn't exist on the 65816 but IR routes any PullReg(PB) here.
        return ["{ uint8 _old_pb = cpu->PB;",
                *(f"  {s}" for s in emitter_helpers.pop_byte_assign(field)),
                *(f"  {s}" for s in widths.set_nz(field, 1)),
                "  cpu_trace_pb_change(cpu, 0, _old_pb, cpu->PB, CPU_TR_PB_WRITE); }"]
    if op.reg == Reg.D:
        # PLD: 16-bit, sets N/Z from popped 16-bit value.
        return emitter_helpers.pop_word_assign(field) + widths.set_nz(field, 2)
    # Final cpu->P sync line — both A and X/Y end with the same packed-flag update.
    p_sync = ("cpu->P = (uint8)((cpu->P & ~0x82) | "
              "(cpu->_flag_Z ? 0x02 : 0) | (cpu->_flag_N ? 0x80 : 0));")
    if op.reg == Reg.A:
        # PLA: width follows M. Preserve B (high byte) in m=1.
        lines = ["if (cpu->m_flag) {",
                 *(f"  {s}" for s in emitter_helpers.pop_byte_assign("uint8 _v")),
                 f"  {field} = {widths.preserve_high(field, '_v')};",
                 *(f"  {s}" for s in widths.set_nz_no_p("_v", 1)),
                 "} else {",
                 *(f"  {s}" for s in emitter_helpers.pop_word_assign(field)),
                 *(f"  {s}" for s in widths.set_nz_no_p(field, 2)),
                 "}",
                 p_sync]
        return lines
    if op.reg in (Reg.X, Reg.Y):
        # PLX/PLY: x=1 zero-extends (hw contract).
        lines = ["if (cpu->x_flag) {",
                 *(f"  {s}" for s in emitter_helpers.pop_byte_assign("uint8 _v")),
                 f"  {field} = {widths.zero_extend_lo('_v')};"
                 f"  /* x=1 zeros high byte (hw contract) */",
                 *(f"  {s}" for s in widths.set_nz_no_p("_v", 1)),
                 "} else {",
                 *(f"  {s}" for s in emitter_helpers.pop_word_assign(field)),
                 *(f"  {s}" for s in widths.set_nz_no_p(field, 2)),
                 "}",
                 p_sync]
        return lines
    return [f"/* TODO PullReg({op.reg}) */"]


def _emit_transfer(op: Transfer) -> List[str]:
    """65816 register-transfer with width-respecting destination AND
    N/Z flag update on the transferred value. TXS / TCS DON'T set
    flags; TCS specifically is a 16-bit copy without flag update.
    Everything else does."""
    src = _reg(op.src)
    dst = _reg(op.dst)
    # TXS, TCS: no flag update, no width check (S is always 16-bit native).
    # TXS only transfers low byte of X (S high stays); TCS transfers all 16
    # bits. v1 emit didn't distinguish. Trace S changes for hunt-the-bug.
    if op.dst == Reg.S:
        return [
            "{ uint16 _old_s = cpu->S;",
            f"  {dst} = {src};",
            "  /* trace_event uses extra0/extra1 for old/new S high bytes */",
            "  cpu_trace_event(cpu, 0, CPU_TR_DB_WRITE,",
            "                  (uint8)(_old_s >> 8), cpu->S); }",
        ]
    # Determine destination width from controlling flag.
    if op.dst == Reg.A:
        flag = "cpu->m_flag"
    elif op.dst in (Reg.X, Reg.Y):
        flag = "cpu->x_flag"
    elif op.dst == Reg.D or op.dst == Reg.S:
        flag = None  # always 16-bit
    else:
        flag = None
    if flag is None:
        # Full-width transfer (D, etc.)
        return [f"{dst} = {src};"] + widths.set_nz(dst, 2)
    # X/Y dest in x=1 zero-extends (high byte hardware-zero); A dest in
    # m=1 preserves high byte (= B register). See _emit_writereg comment
    # for the LoadStripeImage failure that motivated this. 2026-04-30.
    if op.dst in (Reg.X, Reg.Y):
        dst_8bit = f"{dst} = {widths.zero_extend_lo('_v')};  /* x=1 zeros high byte (hw contract) */"
    else:
        dst_8bit = f"{dst} = {widths.preserve_high(dst, '_v')};"
    lines = [f"if ({flag}) {{",
             f"  uint8 _v = {widths.low_byte(src)};",
             f"  {dst_8bit}"]
    lines.extend(f"  {s}" for s in widths.set_nz_no_p("_v", 1))
    lines.append("} else {")
    lines.append(f"  {dst} = (uint16)({src});")
    lines.extend(f"  {s}" for s in widths.set_nz_no_p(dst, 2))
    lines.append("}")
    # Final cpu->P sync after both branches.
    lines.append("cpu->P = (uint8)((cpu->P & ~0x82) | "
                 "(cpu->_flag_Z ? 0x02 : 0) | (cpu->_flag_N ? 0x80 : 0));")
    return lines


def _emit_condbranch(op: CondBranch) -> List[str]:
    pred = f"{_reg(op.flag)} == {op.take_if}"
    # The actual goto target is encoded by the caller (block-level emit) since
    # the IR op itself doesn't store the target — the cfg edge does.
    return [f"if ({pred}) {{ /* take branch — caller fills label */ }}"]


def _emit_goto(op: Goto) -> List[str]:
    # Caller (block-level emit) fills the goto target.
    return ["/* Goto — caller fills label */"]


def _emit_indirect_goto(op: IndirectGoto) -> List[str]:
    bank, addr = _segref_addr_expr(op.seg)
    return [f"/* IndirectGoto: target = ({bank}, {addr}) — caller dispatches */"]


def _emit_dispatch(insn) -> List[str]:
    """Emit a JSL-jump-table dispatch as a static function-pointer
    array indexed by A. The 65816 dispatcher pops its return PC,
    indexes the table at that PC by A (×2 for short, ×3 for long),
    and JMPs through. Effective semantics: select handler by A then
    call. After return, this insn is a TERMINATOR (control returns
    to JSL's caller's caller, not to the bytes after this JSL).

    For each table entry:
      - non-zero, in this bank: emit handler call by friendly name
        (or synthetic bank_BB_AAAA), update PB save/restore, etc.
      - zero: emit a `default: break;` which becomes RTS-style return
    """
    bank = (insn.addr >> 16) & 0xFF
    entries = insn.dispatch_entries
    kind = getattr(insn, 'dispatch_kind', 'short')
    n = len(entries)
    # The dispatched handlers are entered with the dispatcher's (m, x)
    # at the JSL site — same rule as _emit_call. The dispatch helper
    # itself doesn't touch M/X before transferring control. Take the
    # JSL insn's m_flag/x_flag (set by the decoder under whichever
    # entry-state reached this body).
    em = getattr(insn, 'm_flag', 1) & 1
    ex = getattr(insn, 'x_flag', 1) & 1
    suffix = _variant_suffix(em, ex)
    lines = ["{ /* JSL dispatch — short=2B / long=3B table */"]
    lines.append(f"  static const uint16 _disp_n = {n};")
    lines.append(f"  uint16 _idx = (uint16){widths.masked('cpu->A', 1)};")
    lines.append("  if (_idx >= _disp_n) { return; /* dispatch OOB */ }")
    lines.append("  switch (_idx) {")
    for i, e in enumerate(entries):
        if e == 0:
            lines.append(f"    case {i}: break;  /* null entry */")
            continue
        if kind == 'long':
            target_bank = (e >> 16) & 0xFF
            local_pc = e & 0xFFFF
            tgt_addr = e
        else:
            target_bank = bank
            local_pc = e & 0xFFFF
            tgt_addr = (bank << 16) | local_pc
        base_name = _NAME_RESOLVER.get(tgt_addr)
        if base_name is None:
            base_name = f"bank_{target_bank:02X}_{local_pc:04X}"
        # Record demand for both resolved and synthetic targets.
        _UNRESOLVED_CALL_TARGETS.add((tgt_addr, em, ex))
        name = f"{base_name}{suffix}"
        # Single-line case body: join the 6 PB-save/restore statements
        # with spaces so the switch case stays readable in the gen.
        env = emitter_helpers.call_with_pb_save(target_bank, name)
        lines.append(f"    case {i}: {{ {' '.join(env)} }} break;")
    lines.append("    default: break;")
    lines.append("  }")
    lines.append("  return; /* dispatch is a terminator */")
    lines.append("}")
    return lines


def _emit_call(op: Call) -> List[str]:
    if op.indirect:
        return ["/* Call indirect — caller dispatches */"]
    if op.target is None:
        return ["/* Call: target unknown — caller dispatches */"]
    addr = op.target & 0xFFFFFF
    suffix = _variant_suffix(op.entry_m, op.entry_x)
    base_name = _NAME_RESOLVER.get(addr)
    if base_name is None:
        bank = (addr >> 16) & 0xFF
        pc = addr & 0xFFFF
        base_name = f"bank_{bank:02X}_{pc:04X}"
    # Always record demand — cfg-named targets need their (m, x)
    # variants discovered too, not just synthetic-named auto-promotes.
    _UNRESOLVED_CALL_TARGETS.add((addr, op.entry_m & 1, op.entry_x & 1))
    name = f"{base_name}{suffix}"
    target_bank = (addr >> 16) & 0xFF
    if op.long:
        # JSL: real hardware sets PB to the target bank for the call's
        # duration, then RTL restores it. Emit explicit PB save/restore
        # so PHK inside the callee pushes the CORRECT bank — without
        # this, PHK; PLB inside a JSL'd function poisons DB to the
        # CALLER's bank instead of the callee's (= currently $00 always).
        env = emitter_helpers.call_with_pb_save(target_bank, name)
        return ["{"] + [f"  {s}" for s in env] + ["}"]
    # JSR: same-bank short call. PB doesn't change.
    return [f"{name}(cpu);"]


def _emit_return(op: Return) -> List[str]:
    if op.interrupt:
        return [
            "cpu_trace_event(cpu, 0, CPU_TR_RTI, 0, 0);",
            "return; /* RTI */",
        ]
    return ["return; /* RTL */" if op.long else "return; /* RTS */"]


def _emit_stop(op: Stop) -> List[str]:
    if op.wait:
        return ["/* WAI: wait for interrupt — runtime hook */"]
    return ["/* STP: halt — runtime hook */"]


def _emit_break(op: Break) -> List[str]:
    return ["/* COP: software interrupt */" if op.cop else "/* BRK: software interrupt */"]


def _emit_nop(op: Nop) -> List[str]:
    return ["/* NOP */"]


def _emit_pea_per_pei(op: PushEffectiveAddress) -> List[str]:
    if op.seg.kind == SegKind.ABS_BANK:
        return [
            "cpu->S = (uint16)(cpu->S - 1);",
            f"cpu_write16(cpu, 0x00, cpu->S, (uint16){op.seg.offset:#06x});",
            "cpu->S = (uint16)(cpu->S - 1);",
        ]
    if op.seg.kind == SegKind.DP_INDIRECT:
        return [
            "{",
            f"  uint16 _peival = cpu_read16(cpu, 0x00, (uint16)(cpu->D + {op.seg.offset:#06x}));",
            "  cpu->S = (uint16)(cpu->S - 1);",
            "  cpu_write16(cpu, 0x00, cpu->S, _peival);",
            "  cpu->S = (uint16)(cpu->S - 1);",
            "}",
        ]
    return ["/* TODO PushEffectiveAddress unsupported kind */"]


def _emit_blockmove(op: BlockMove) -> List[str]:
    delta = "+1" if op.direction == "mvn" else "-1"
    et = "CPU_TR_MVN" if op.direction == "mvn" else "CPU_TR_MVP"
    return [
        "{",
        f"  uint8 _src_b = {op.src_bank:#04x};",
        f"  uint8 _dst_b = {op.dst_bank:#04x};",
        "  uint8 _old_db = cpu->DB;",
        f"  cpu_trace_event(cpu, 0, {et}, _src_b, _dst_b);",
        "  while (cpu->A != 0xFFFF) {",
        "    uint8 _b = cpu_read8(cpu, _src_b, cpu->X);",
        "    cpu_write8(cpu, _dst_b, cpu->Y, _b);",
        f"    cpu->X = (uint16)(cpu->X {delta});",
        f"    cpu->Y = (uint16)(cpu->Y {delta});",
        "    cpu->A = (uint16)(cpu->A - 1);",
        "  }",
        "  cpu->DB = _dst_b;",
        f"  cpu_trace_db_change(cpu, 0, _old_db, _dst_b, {et});",
        "}",
    ]


def _emit_push(op: Push) -> List[str]:
    if op.width == 1:
        return emitter_helpers.push_byte(f"(uint8){_v(op.src)}")
    return emitter_helpers.push_word(_v(op.src))


def _emit_pull(op: Pull) -> List[str]:
    if op.width == 1:
        return emitter_helpers.pop_byte_assign(f"uint8 {_v(op.out)}")
    return emitter_helpers.pop_word_assign(f"uint16 {_v(op.out)}")


# ── Dispatch ────────────────────────────────────────────────────────────────

_DISPATCH = {
    Read: _emit_read, Write: _emit_write,
    ReadReg: _emit_readreg, WriteReg: _emit_writereg,
    ConstI: _emit_consti,
    Alu: _emit_alu, Shift: _emit_shift, IncReg: _emit_increg, IncMem: _emit_incmem,
    BitTest: _emit_bittest, BitSetMem: _emit_bitsetmem, BitClearMem: _emit_bitclearmem,
    SetFlag: _emit_setflag, SetNZ: _emit_setnz,
    RepFlags: _emit_repflags, SepFlags: _emit_sepflags,
    XCE: _emit_xce, XBA: _emit_xba,
    Push: _emit_push, Pull: _emit_pull,
    PushReg: _emit_pushreg, PullReg: _emit_pullreg,
    BlockMove: _emit_blockmove,
    CondBranch: _emit_condbranch, Goto: _emit_goto,
    IndirectGoto: _emit_indirect_goto, Call: _emit_call,
    Return: _emit_return, Transfer: _emit_transfer,
    Nop: _emit_nop, Break: _emit_break, Stop: _emit_stop,
    PushEffectiveAddress: _emit_pea_per_pei,
}


def emit_op(op: IROp) -> List[str]:
    """Lower a single IR op to one or more lines of C."""
    h = _DISPATCH.get(type(op))
    if h is None:
        return [f"/* UNHANDLED IR op {type(op).__name__} */"]
    return [ln for ln in h(op) if ln]


def emit_block(block: IRBlock, *, indent: str = "  ") -> List[str]:
    """Emit a list of indented C lines for one IRBlock.

    The block is wrapped in `{ ... }` so locals (introduced by ConstI,
    Read, ReadReg, Pull) don't leak across blocks.
    """
    lines = ["{"]
    for op in block.ops:
        for ln in emit_op(op):
            lines.append(indent + ln)
    lines.append("}")
    return lines
