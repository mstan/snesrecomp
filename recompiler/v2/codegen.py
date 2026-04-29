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

from typing import List  # noqa: E402

from v2.ir import (  # noqa: E402
    IROp, IRBlock,
    Read, Write, ReadReg, WriteReg, ConstI,
    Alu, AluOp, Shift, ShiftOp, IncReg,
    BitTest, BitSetMem, BitClearMem,
    SetFlag, RepFlags, SepFlags, XCE,
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
_REG_FIELD = {
    Reg.A: "cpu->A", Reg.B: "cpu->B",
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
    fn = "cpu_read8" if op.width == 1 else "cpu_read16"
    return [f"{_ctype(op.width)} {_v(op.out)} = {fn}(cpu, {bank}, {addr});"]


def _emit_write(op: Write) -> List[str]:
    bank, addr = _segref_addr_expr(op.seg)
    fn = "cpu_write8" if op.width == 1 else "cpu_write16"
    return [f"{fn}(cpu, {bank}, {addr}, {_v(op.src)});"]


def _emit_readreg(op: ReadReg) -> List[str]:
    return [f"uint16 {_v(op.out)} = (uint16){_reg(op.reg)};"]


def _emit_writereg(op: WriteReg) -> List[str]:
    return [f"{_reg(op.reg)} = {_v(op.src)};"]


def _emit_consti(op: ConstI) -> List[str]:
    return [f"{_ctype(op.width)} {_v(op.out)} = {op.value:#x};"]


def _emit_alu(op: Alu) -> List[str]:
    lines = []
    if op.op == AluOp.ADD:
        # ADC: lhs + rhs + C; updates N/V/Z/C.
        lines.append(
            f"uint32 _t = (uint32){_v(op.lhs)} + (uint32){_v(op.rhs)} + cpu->_flag_C;"
        )
        if op.out is not None:
            lines.append(f"{_ctype(op.width)} {_v(op.out)} = ({_ctype(op.width)})_t;")
        mask = "0x100" if op.width == 1 else "0x10000"
        lines.append(f"cpu->_flag_C = (_t & {mask}) ? 1 : 0;")
    elif op.op == AluOp.SUB:
        # SBC: lhs - rhs - !C; updates N/V/Z/C.
        lines.append(
            f"uint32 _t = (uint32){_v(op.lhs)} - (uint32){_v(op.rhs)} - (1 - cpu->_flag_C);"
        )
        if op.out is not None:
            lines.append(f"{_ctype(op.width)} {_v(op.out)} = ({_ctype(op.width)})_t;")
        mask = "0x100" if op.width == 1 else "0x10000"
        lines.append(f"cpu->_flag_C = (_t & {mask}) ? 0 : 1;")
    elif op.op == AluOp.AND:
        lines.append(
            f"{_ctype(op.width)} {_v(op.out)} = "
            f"({_ctype(op.width)})({_v(op.lhs)} & {_v(op.rhs)});"
        )
    elif op.op == AluOp.OR:
        lines.append(
            f"{_ctype(op.width)} {_v(op.out)} = "
            f"({_ctype(op.width)})({_v(op.lhs)} | {_v(op.rhs)});"
        )
    elif op.op == AluOp.XOR:
        lines.append(
            f"{_ctype(op.width)} {_v(op.out)} = "
            f"({_ctype(op.width)})({_v(op.lhs)} ^ {_v(op.rhs)});"
        )
    elif op.op == AluOp.CMP:
        # No destination; flags only.
        lines.append(
            f"uint32 _t = (uint32){_v(op.lhs)} - (uint32){_v(op.rhs)};"
        )
        sign = "0x80" if op.width == 1 else "0x8000"
        lines.append(f"cpu->_flag_C = ({_v(op.lhs)} >= {_v(op.rhs)}) ? 1 : 0;")
        lines.append(f"cpu->_flag_Z = (({_ctype(op.width)})_t == 0) ? 1 : 0;")
        lines.append(f"cpu->_flag_N = ((_t & {sign}) != 0) ? 1 : 0;")
        return lines  # CMP doesn't set N/Z below

    # Common N/Z update for non-CMP ops.
    if op.out is not None:
        sign = "0x80" if op.width == 1 else "0x8000"
        lines.append(f"cpu->_flag_Z = ({_v(op.out)} == 0) ? 1 : 0;")
        lines.append(f"cpu->_flag_N = (({_v(op.out)} & {sign}) != 0) ? 1 : 0;")
    return lines


def _emit_shift(op: Shift) -> List[str]:
    sign = "0x80" if op.width == 1 else "0x8000"
    if op.op == ShiftOp.ASL:
        return [
            f"{_ctype(op.width)} {_v(op.out)} = ({_ctype(op.width)})({_v(op.src)} << 1);",
            f"cpu->_flag_C = (({_v(op.src)} & {sign}) != 0) ? 1 : 0;",
            f"cpu->_flag_Z = ({_v(op.out)} == 0) ? 1 : 0;",
            f"cpu->_flag_N = (({_v(op.out)} & {sign}) != 0) ? 1 : 0;",
        ]
    if op.op == ShiftOp.LSR:
        return [
            f"{_ctype(op.width)} {_v(op.out)} = ({_ctype(op.width)})({_v(op.src)} >> 1);",
            f"cpu->_flag_C = ({_v(op.src)} & 1) ? 1 : 0;",
            f"cpu->_flag_Z = ({_v(op.out)} == 0) ? 1 : 0;",
            f"cpu->_flag_N = (({_v(op.out)} & {sign}) != 0) ? 1 : 0;",
        ]
    if op.op == ShiftOp.ROL:
        return [
            f"{_ctype(op.width)} {_v(op.out)} = "
            f"({_ctype(op.width)})(({_v(op.src)} << 1) | cpu->_flag_C);",
            f"cpu->_flag_C = (({_v(op.src)} & {sign}) != 0) ? 1 : 0;",
            f"cpu->_flag_Z = ({_v(op.out)} == 0) ? 1 : 0;",
            f"cpu->_flag_N = (({_v(op.out)} & {sign}) != 0) ? 1 : 0;",
        ]
    if op.op == ShiftOp.ROR:
        return [
            f"{_ctype(op.width)} {_v(op.out)} = "
            f"({_ctype(op.width)})(({_v(op.src)} >> 1) | "
            f"((uint{op.width*8})cpu->_flag_C << {op.width * 8 - 1}));",
            f"cpu->_flag_C = ({_v(op.src)} & 1) ? 1 : 0;",
            f"cpu->_flag_Z = ({_v(op.out)} == 0) ? 1 : 0;",
            f"cpu->_flag_N = (({_v(op.out)} & {sign}) != 0) ? 1 : 0;",
        ]
    raise ValueError(f"unhandled Shift op {op.op}")


def _emit_increg(op: IncReg) -> List[str]:
    field = _reg(op.reg)
    delta = "1" if op.delta == +1 else "-1"
    return [
        f"{field} = ({field}) + ({delta});",
        # N/Z update uses the appropriate width for the register.
        f"cpu->_flag_Z = ({field} == 0) ? 1 : 0;",
        f"cpu->_flag_N = (({field} & 0x8000) != 0) ? 1 : 0;",
    ]


def _emit_bittest(op: BitTest) -> List[str]:
    sign = "0x80" if op.width == 1 else "0x8000"
    overflow = "0x40" if op.width == 1 else "0x4000"
    return [
        f"{_ctype(op.width)} _bt = ({_ctype(op.width)})(cpu->A & {_v(op.operand)});",
        f"cpu->_flag_Z = (_bt == 0) ? 1 : 0;",
        f"cpu->_flag_N = (({_v(op.operand)} & {sign}) != 0) ? 1 : 0;",
        f"cpu->_flag_V = (({_v(op.operand)} & {overflow}) != 0) ? 1 : 0;",
    ]


def _emit_bitsetmem(op: BitSetMem) -> List[str]:
    bank, addr = _segref_addr_expr(op.seg)
    fn_r = "cpu_read8" if op.width == 1 else "cpu_read16"
    fn_w = "cpu_write8" if op.width == 1 else "cpu_write16"
    return [
        f"{_ctype(op.width)} _m = {fn_r}(cpu, {bank}, {addr});",
        f"cpu->_flag_Z = ((_m & cpu->A) == 0) ? 1 : 0;",
        f"{fn_w}(cpu, {bank}, {addr}, ({_ctype(op.width)})(_m | cpu->A));",
    ]


def _emit_bitclearmem(op: BitClearMem) -> List[str]:
    bank, addr = _segref_addr_expr(op.seg)
    fn_r = "cpu_read8" if op.width == 1 else "cpu_read16"
    fn_w = "cpu_write8" if op.width == 1 else "cpu_write16"
    return [
        f"{_ctype(op.width)} _m = {fn_r}(cpu, {bank}, {addr});",
        f"cpu->_flag_Z = ((_m & cpu->A) == 0) ? 1 : 0;",
        f"{fn_w}(cpu, {bank}, {addr}, ({_ctype(op.width)})(_m & ~cpu->A));",
    ]


def _emit_setflag(op: SetFlag) -> List[str]:
    return [f"{_reg(op.flag)} = {op.value};"]


def _emit_repflags(op: RepFlags) -> List[str]:
    return [
        f"cpu->P = (uint8)(cpu->P & ~{op.mask:#04x});",
        "cpu_p_to_mirrors(cpu);",
    ]


def _emit_sepflags(op: SepFlags) -> List[str]:
    return [
        f"cpu->P = (uint8)(cpu->P | {op.mask:#04x});",
        "cpu_p_to_mirrors(cpu);",
    ]


def _emit_xce(op: XCE) -> List[str]:
    return [
        "{",
        "  uint8 _t = cpu->emulation;",
        "  cpu->emulation = cpu->_flag_C;",
        "  cpu->_flag_C = _t;",
        "  if (cpu->emulation) { cpu->m_flag = 1; cpu->x_flag = 1; cpu_mirrors_to_p(cpu); }",
        "}",
    ]


def _emit_xba(op: XBA) -> List[str]:
    return [
        "{",
        "  uint8 _lo = (uint8)(cpu->A & 0xFF);",
        "  cpu->A = (uint16)((uint16)cpu->B | ((uint16)_lo << 8));",
        "  cpu->B = (uint8)((cpu->A >> 8) & 0xFF);",  # B mirrors A high
        # Z/N from new low byte
        "  cpu->_flag_Z = ((cpu->A & 0xFF) == 0) ? 1 : 0;",
        "  cpu->_flag_N = ((cpu->A & 0x80) != 0) ? 1 : 0;",
        "}",
    ]


def _emit_pushreg(op: PushReg) -> List[str]:
    field = _reg(op.reg)
    # Push is 1 or 2 bytes depending on register; for now treat A/B/X/Y/D as
    # following m/x widths and S/DB/PB as 1-byte. P is 1 byte. D is 16-bit.
    if op.reg in (Reg.DB, Reg.PB, Reg.P):
        return [
            f"cpu_write8(cpu, 0x00, cpu->S, (uint8)({field}));",
            "cpu->S = (uint16)(cpu->S - 1);",
        ]
    if op.reg == Reg.D:
        return [
            f"cpu->S = (uint16)(cpu->S - 1);",
            f"cpu_write16(cpu, 0x00, cpu->S, {field});",
            f"cpu->S = (uint16)(cpu->S - 1);",
        ]
    # A/B/X/Y: width depends on M/X flag.
    if op.reg == Reg.A:
        return [
            "if (cpu->m_flag) {",
            f"  cpu_write8(cpu, 0x00, cpu->S, (uint8)({field} & 0xFF));",
            "  cpu->S = (uint16)(cpu->S - 1);",
            "} else {",
            "  cpu->S = (uint16)(cpu->S - 1);",
            f"  cpu_write16(cpu, 0x00, cpu->S, {field});",
            "  cpu->S = (uint16)(cpu->S - 1);",
            "}",
        ]
    if op.reg in (Reg.X, Reg.Y):
        return [
            "if (cpu->x_flag) {",
            f"  cpu_write8(cpu, 0x00, cpu->S, (uint8)({field} & 0xFF));",
            "  cpu->S = (uint16)(cpu->S - 1);",
            "} else {",
            "  cpu->S = (uint16)(cpu->S - 1);",
            f"  cpu_write16(cpu, 0x00, cpu->S, {field});",
            "  cpu->S = (uint16)(cpu->S - 1);",
            "}",
        ]
    return [f"/* TODO PushReg({op.reg}) */"]


def _emit_pullreg(op: PullReg) -> List[str]:
    field = _reg(op.reg)
    if op.reg in (Reg.DB, Reg.PB, Reg.P):
        return [
            "cpu->S = (uint16)(cpu->S + 1);",
            f"{field} = cpu_read8(cpu, 0x00, cpu->S);",
            "cpu_p_to_mirrors(cpu);" if op.reg == Reg.P else "",
        ]
    if op.reg == Reg.D:
        return [
            "cpu->S = (uint16)(cpu->S + 1);",
            f"{field} = cpu_read16(cpu, 0x00, cpu->S);",
            "cpu->S = (uint16)(cpu->S + 1);",
        ]
    if op.reg == Reg.A:
        return [
            "if (cpu->m_flag) {",
            "  cpu->S = (uint16)(cpu->S + 1);",
            f"  {field} = (uint16)(({field} & 0xFF00) | cpu_read8(cpu, 0x00, cpu->S));",
            "} else {",
            "  cpu->S = (uint16)(cpu->S + 1);",
            f"  {field} = cpu_read16(cpu, 0x00, cpu->S);",
            "  cpu->S = (uint16)(cpu->S + 1);",
            "}",
        ]
    if op.reg in (Reg.X, Reg.Y):
        return [
            "if (cpu->x_flag) {",
            "  cpu->S = (uint16)(cpu->S + 1);",
            f"  {field} = (uint16)cpu_read8(cpu, 0x00, cpu->S);",
            "} else {",
            "  cpu->S = (uint16)(cpu->S + 1);",
            f"  {field} = cpu_read16(cpu, 0x00, cpu->S);",
            "  cpu->S = (uint16)(cpu->S + 1);",
            "}",
        ]
    return [f"/* TODO PullReg({op.reg}) */"]


def _emit_transfer(op: Transfer) -> List[str]:
    return [f"{_reg(op.dst)} = {_reg(op.src)};"]


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


def _emit_call(op: Call) -> List[str]:
    if op.indirect:
        return ["/* Call indirect — caller dispatches */"]
    if op.target is None:
        return ["/* Call: target unknown — caller dispatches */"]
    bank = (op.target >> 16) & 0xFF
    pc = op.target & 0xFFFF
    if op.long:
        return [f"bank_{bank:02X}_{pc:04X}(cpu);"]
    # Short JSR: same-bank call. Caller's bank should match insn.addr's bank.
    return [f"bank_{bank:02X}_{pc:04X}(cpu);"]


def _emit_return(op: Return) -> List[str]:
    if op.interrupt:
        return ["return; /* RTI */"]
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
            f"uint16 _v = cpu_read16(cpu, 0x00, (uint16)(cpu->D + {op.seg.offset:#06x}));",
            "cpu->S = (uint16)(cpu->S - 1);",
            "cpu_write16(cpu, 0x00, cpu->S, _v);",
            "cpu->S = (uint16)(cpu->S - 1);",
        ]
    return ["/* TODO PushEffectiveAddress unsupported kind */"]


def _emit_blockmove(op: BlockMove) -> List[str]:
    delta = "+1" if op.direction == "mvn" else "-1"
    return [
        "{",
        f"  uint8 _src_b = {op.src_bank:#04x};",
        f"  uint8 _dst_b = {op.dst_bank:#04x};",
        "  while (cpu->A != 0xFFFF) {",
        "    uint8 _b = cpu_read8(cpu, _src_b, cpu->X);",
        "    cpu_write8(cpu, _dst_b, cpu->Y, _b);",
        f"    cpu->X = (uint16)(cpu->X {delta});",
        f"    cpu->Y = (uint16)(cpu->Y {delta});",
        "    cpu->A = (uint16)(cpu->A - 1);",
        "  }",
        "  cpu->DB = _dst_b;",
        "}",
    ]


def _emit_push(op: Push) -> List[str]:
    if op.width == 1:
        return [
            f"cpu_write8(cpu, 0x00, cpu->S, (uint8){_v(op.src)});",
            "cpu->S = (uint16)(cpu->S - 1);",
        ]
    return [
        "cpu->S = (uint16)(cpu->S - 1);",
        f"cpu_write16(cpu, 0x00, cpu->S, {_v(op.src)});",
        "cpu->S = (uint16)(cpu->S - 1);",
    ]


def _emit_pull(op: Pull) -> List[str]:
    if op.width == 1:
        return [
            "cpu->S = (uint16)(cpu->S + 1);",
            f"uint8 {_v(op.out)} = cpu_read8(cpu, 0x00, cpu->S);",
        ]
    return [
        "cpu->S = (uint16)(cpu->S + 1);",
        f"uint16 {_v(op.out)} = cpu_read16(cpu, 0x00, cpu->S);",
        "cpu->S = (uint16)(cpu->S + 1);",
    ]


# ── Dispatch ────────────────────────────────────────────────────────────────

_DISPATCH = {
    Read: _emit_read, Write: _emit_write,
    ReadReg: _emit_readreg, WriteReg: _emit_writereg,
    ConstI: _emit_consti,
    Alu: _emit_alu, Shift: _emit_shift, IncReg: _emit_increg,
    BitTest: _emit_bittest, BitSetMem: _emit_bitsetmem, BitClearMem: _emit_bitclearmem,
    SetFlag: _emit_setflag, RepFlags: _emit_repflags, SepFlags: _emit_sepflags,
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
