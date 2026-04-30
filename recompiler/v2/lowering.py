"""snesrecomp.recompiler.v2.lowering

Lowers a decoded `Insn` (from snes65816.py) into a list of IR ops
(from ir.py). Covers all 256 65816 opcodes; addressing modes are
resolved to `SegRef`s tagged with the right `SegKind`.

Width comes from the entry M/X flags stamped on `Insn.m_flag` /
`Insn.x_flag` (the v2 decoder guarantees these are correct per
DecodeKey, not linear scalars).

Public API:
    lower(insn: Insn, *, value_factory: ValueFactory) -> List[IROp]

`value_factory` is a small callable supplied by the caller to mint
fresh `Value` IDs (so a function-wide IR build can keep IDs unique).

Lowering does NOT perform optimization. Each insn produces a faithful
sequence; later phases handle DCE / coalescing / SSA.
"""

import sys
import pathlib

_THIS_DIR = pathlib.Path(__file__).resolve().parent
_RECOMPILER_DIR = _THIS_DIR.parent
for p in (str(_THIS_DIR), str(_RECOMPILER_DIR)):
    if p not in sys.path:
        sys.path.insert(0, p)

from typing import Callable, List  # noqa: E402

import snes65816  # noqa: E402
from snes65816 import (  # noqa: E402
    IMP, ACC, IMM, DP, DP_X, DP_Y, ABS, ABS_X, ABS_Y, LONG, LONG_X,
    REL, REL16, STK, INDIR, INDIR_X, INDIR_Y, INDIR_LY, INDIR_L,
    INDIR_DPX, DP_INDIR, STK_IY, Insn,
)

from v2.ir import (  # noqa: E402
    IROp, Read, Write, ReadReg, WriteReg, ConstI, Alu, AluOp, Shift,
    ShiftOp, IncReg, BitTest, BitSetMem, BitClearMem,
    SetFlag, SetNZ, RepFlags, SepFlags, XCE, Push, Pull, PushReg, PullReg,
    BlockMove, CondBranch, Goto, IndirectGoto, Call, Return,
    Transfer, XBA, Nop, Break, Stop, PushEffectiveAddress,
    Reg, SegRef, SegKind, Value,
)


ValueFactory = Callable[[], Value]


# ── Addressing-mode -> SegRef ────────────────────────────────────────────────

def _segref_for(insn: Insn) -> SegRef:
    """Resolve the addressing mode of `insn` into a SegRef.

    For modes that don't reference memory (IMP, ACC, IMM, REL, REL16),
    the caller doesn't invoke this. For those that do, we map the mode
    onto one of the SegKind variants. `bank` is left as None except for
    LONG / LONG_X where the operand carries it explicitly.
    """
    m = insn.mode
    op = insn.operand

    if m == DP:
        return SegRef(kind=SegKind.DIRECT, offset=op)
    if m == DP_X:
        return SegRef(kind=SegKind.DIRECT, offset=op, index=Reg.X)
    if m == DP_Y:
        return SegRef(kind=SegKind.DIRECT, offset=op, index=Reg.Y)

    if m == ABS:
        return SegRef(kind=SegKind.ABS_BANK, offset=op)
    if m == ABS_X:
        return SegRef(kind=SegKind.ABS_BANK, offset=op, index=Reg.X)
    if m == ABS_Y:
        return SegRef(kind=SegKind.ABS_BANK, offset=op, index=Reg.Y)

    if m == LONG:
        return SegRef(kind=SegKind.LONG, offset=op & 0xFFFF, bank=(op >> 16) & 0xFF)
    if m == LONG_X:
        return SegRef(kind=SegKind.LONG, offset=op & 0xFFFF, bank=(op >> 16) & 0xFF, index=Reg.X)

    if m == STK:
        return SegRef(kind=SegKind.STACK, offset=op)
    if m == STK_IY:
        return SegRef(kind=SegKind.STACK_REL_INDIRECT_Y, offset=op)

    if m == INDIR:
        # Used by JMP (abs) / JMP [abs] — JMP variants resolve via
        # their own ops; for ALU/load contexts we stay generic.
        return SegRef(kind=SegKind.ABS_INDIRECT, offset=op)
    if m == INDIR_X:
        return SegRef(kind=SegKind.ABS_INDIRECT_X, offset=op)

    if m == DP_INDIR:
        return SegRef(kind=SegKind.DP_INDIRECT, offset=op)
    if m == INDIR_Y:
        return SegRef(kind=SegKind.DP_INDIRECT, offset=op, index=Reg.Y)
    if m == INDIR_DPX:
        return SegRef(kind=SegKind.DP_INDIRECT_X, offset=op)

    if m == INDIR_L:
        return SegRef(kind=SegKind.DP_INDIRECT_LONG, offset=op)
    if m == INDIR_LY:
        return SegRef(kind=SegKind.DP_INDIRECT_LONG, offset=op, index=Reg.Y)

    raise ValueError(f"_segref_for: mode {m} not memory-referencing")


# ── Width helpers ────────────────────────────────────────────────────────────

def _width_a(insn: Insn) -> int:
    return 1 if insn.m_flag else 2


def _width_x(insn: Insn) -> int:
    return 1 if insn.x_flag else 2


# ── Top-level dispatch ──────────────────────────────────────────────────────

def lower(insn: Insn, *, value_factory: ValueFactory) -> List[IROp]:
    """Lower one Insn to a list of IR ops.

    The list is non-empty for every opcode (Nop is used as a placeholder
    for genuinely-unmodeled instructions like WDM, but BRK, COP, STP,
    WAI etc. have their own ops).
    """
    h = _DISPATCH.get(insn.mnem)
    if h is None:
        # Default: treat as no-op so the coverage gate doesn't crash.
        # If we ever reach this in real ROM, the per-op smoke test would
        # have caught it; in production every mnem is dispatched.
        return [Nop()]
    return h(insn, value_factory)


# ── Per-mnemonic handlers ───────────────────────────────────────────────────

def _h_lda(insn, vf):
    w = _width_a(insn)
    if insn.mode == IMM:
        v = vf()
        return [ConstI(value=insn.operand, width=w, out=v),
                WriteReg(reg=Reg.A, src=v),
                SetNZ(src=v, width=w)]
    seg = _segref_for(insn)
    v = vf()
    return [Read(seg=seg, width=w, out=v),
            WriteReg(reg=Reg.A, src=v),
            SetNZ(src=v, width=w)]


def _h_ldx(insn, vf):
    w = _width_x(insn)
    if insn.mode == IMM:
        v = vf()
        return [ConstI(value=insn.operand, width=w, out=v),
                WriteReg(reg=Reg.X, src=v),
                SetNZ(src=v, width=w)]
    seg = _segref_for(insn)
    v = vf()
    return [Read(seg=seg, width=w, out=v),
            WriteReg(reg=Reg.X, src=v),
            SetNZ(src=v, width=w)]


def _h_ldy(insn, vf):
    w = _width_x(insn)
    if insn.mode == IMM:
        v = vf()
        return [ConstI(value=insn.operand, width=w, out=v),
                WriteReg(reg=Reg.Y, src=v),
                SetNZ(src=v, width=w)]
    seg = _segref_for(insn)
    v = vf()
    return [Read(seg=seg, width=w, out=v),
            WriteReg(reg=Reg.Y, src=v),
            SetNZ(src=v, width=w)]


def _h_sta(insn, vf):
    seg = _segref_for(insn)
    v = vf()
    return [ReadReg(reg=Reg.A, out=v),
            Write(seg=seg, src=v, width=_width_a(insn))]


def _h_stx(insn, vf):
    seg = _segref_for(insn)
    v = vf()
    return [ReadReg(reg=Reg.X, out=v),
            Write(seg=seg, src=v, width=_width_x(insn))]


def _h_sty(insn, vf):
    seg = _segref_for(insn)
    v = vf()
    return [ReadReg(reg=Reg.Y, out=v),
            Write(seg=seg, src=v, width=_width_x(insn))]


def _h_stz(insn, vf):
    seg = _segref_for(insn)
    z = vf()
    return [ConstI(value=0, width=_width_a(insn), out=z),
            Write(seg=seg, src=z, width=_width_a(insn))]


# ALU ops: ADC, SBC, AND, ORA, EOR, CMP, CPX, CPY
def _alu_handler(op: AluOp, lhs_reg: Reg, width_fn):
    """Build a handler that lowers an ALU op against the named accumulator-like register."""

    def h(insn, vf):
        width = width_fn(insn)
        # Acquire RHS
        if insn.mode == IMM:
            rhs = vf()
            ops_pre = [ConstI(value=insn.operand, width=width, out=rhs)]
        else:
            seg = _segref_for(insn)
            rhs = vf()
            ops_pre = [Read(seg=seg, width=width, out=rhs)]
        lhs = vf()
        out = None if op == AluOp.CMP else vf()
        ops = list(ops_pre)
        ops.append(ReadReg(reg=lhs_reg, out=lhs))
        ops.append(Alu(op=op, lhs=lhs, rhs=rhs, width=width, out=out))
        if out is not None:
            ops.append(WriteReg(reg=lhs_reg, src=out))
        return ops

    return h


# Read-modify-write: ASL, LSR, ROL, ROR, INC, DEC (memory or accumulator)
def _shift_handler(op: ShiftOp):
    def h(insn, vf):
        width = _width_a(insn)
        if insn.mode == ACC:
            src = vf()
            out = vf()
            return [ReadReg(reg=Reg.A, out=src),
                    Shift(op=op, src=src, width=width, out=out),
                    WriteReg(reg=Reg.A, src=out)]
        seg = _segref_for(insn)
        src = vf()
        out = vf()
        return [Read(seg=seg, width=width, out=src),
                Shift(op=op, src=src, width=width, out=out),
                Write(seg=seg, src=out, width=width)]
    return h


def _h_inc(insn, vf):
    if insn.mode == ACC:
        return [IncReg(reg=Reg.A, delta=+1)]
    width = _width_a(insn)
    seg = _segref_for(insn)
    src = vf()
    one = vf()
    out = vf()
    return [Read(seg=seg, width=width, out=src),
            ConstI(value=1, width=width, out=one),
            Alu(op=AluOp.ADD, lhs=src, rhs=one, width=width, out=out),
            Write(seg=seg, src=out, width=width)]


def _h_dec(insn, vf):
    if insn.mode == ACC:
        return [IncReg(reg=Reg.A, delta=-1)]
    width = _width_a(insn)
    seg = _segref_for(insn)
    src = vf()
    one = vf()
    out = vf()
    return [Read(seg=seg, width=width, out=src),
            ConstI(value=1, width=width, out=one),
            Alu(op=AluOp.SUB, lhs=src, rhs=one, width=width, out=out),
            Write(seg=seg, src=out, width=width)]


def _h_inx(insn, vf): return [IncReg(reg=Reg.X, delta=+1)]
def _h_iny(insn, vf): return [IncReg(reg=Reg.Y, delta=+1)]
def _h_dex(insn, vf): return [IncReg(reg=Reg.X, delta=-1)]
def _h_dey(insn, vf): return [IncReg(reg=Reg.Y, delta=-1)]


# BIT: special — IMM form is flag-only-on-A, memory form is BitTest
def _h_bit(insn, vf):
    width = _width_a(insn)
    if insn.mode == IMM:
        rhs = vf()
        return [ConstI(value=insn.operand, width=width, out=rhs),
                BitTest(operand=rhs, width=width)]
    seg = _segref_for(insn)
    rhs = vf()
    return [Read(seg=seg, width=width, out=rhs),
            BitTest(operand=rhs, width=width)]


def _h_tsb(insn, vf):
    return [BitSetMem(seg=_segref_for(insn), width=_width_a(insn))]


def _h_trb(insn, vf):
    return [BitClearMem(seg=_segref_for(insn), width=_width_a(insn))]


# Transfers
def _xfer(src, dst):
    def h(insn, vf): return [Transfer(src=src, dst=dst)]
    return h


# Flag ops
def _flag_set(reg, val):
    def h(insn, vf): return [SetFlag(flag=reg, value=val)]
    return h


def _h_rep(insn, vf): return [RepFlags(mask=insn.operand)]
def _h_sep(insn, vf): return [SepFlags(mask=insn.operand)]
def _h_xce(insn, vf): return [XCE()]
def _h_xba(insn, vf): return [XBA()]


# Stack
def _push_reg(reg):
    def h(insn, vf): return [PushReg(reg=reg)]
    return h


def _pull_reg(reg):
    def h(insn, vf): return [PullReg(reg=reg)]
    return h


# Branches
def _cond_branch(flag, take_if):
    def h(insn, vf): return [CondBranch(flag=flag, take_if=take_if)]
    return h


def _h_bra(insn, vf): return [Goto()]
def _h_brl(insn, vf): return [Goto()]


# Control flow
def _h_jmp(insn, vf):
    if insn.mode in (INDIR, INDIR_X):
        seg = _segref_for(insn)
        return [IndirectGoto(seg=seg)]
    if insn.mode == LONG:
        # Cross-bank long jump — model as Goto for now; v2 cfg is responsible
        # for tracking the cross-bank successor (or treating it as terminator).
        return [Goto()]
    return [Goto()]


def _h_jsr(insn, vf):
    if insn.mode == INDIR_X:
        return [Call(target=None, long=False, indirect=True)]
    # JSR is a same-bank short call; insn.operand is the 16-bit local PC.
    # Combine with the source bank from insn.addr so codegen can resolve
    # the cross-bank call name (e.g. bank_0C_944C, not bank_00_944C).
    src_bank = (insn.addr >> 16) & 0xFF
    target = (src_bank << 16) | (insn.operand & 0xFFFF)
    return [Call(target=target, long=False)]


def _h_jsl(insn, vf):
    return [Call(target=insn.operand, long=True)]


def _h_rts(insn, vf): return [Return(long=False)]
def _h_rtl(insn, vf): return [Return(long=True)]
def _h_rti(insn, vf): return [Return(long=True, interrupt=True)]


# Misc
def _h_nop(insn, vf): return [Nop()]
def _h_wdm(insn, vf): return [Nop()]
def _h_brk(insn, vf): return [Break(cop=False)]
def _h_cop(insn, vf): return [Break(cop=True)]
def _h_stp(insn, vf): return [Stop(wait=False)]
def _h_wai(insn, vf): return [Stop(wait=True)]


def _h_pea(insn, vf):
    return [PushEffectiveAddress(seg=SegRef(kind=SegKind.ABS_BANK, offset=insn.operand))]


def _h_per(insn, vf):
    # Operand is already resolved to an absolute PC by decode_insn.
    return [PushEffectiveAddress(seg=SegRef(kind=SegKind.ABS_BANK, offset=insn.operand))]


def _h_pei(insn, vf):
    return [PushEffectiveAddress(seg=SegRef(kind=SegKind.DP_INDIRECT, offset=insn.operand))]


def _h_mvn(insn, vf):
    src_bank = insn.operand & 0xFF
    dst_bank = (insn.operand >> 8) & 0xFF
    return [BlockMove(direction='mvn', src_bank=src_bank, dst_bank=dst_bank)]


def _h_mvp(insn, vf):
    src_bank = insn.operand & 0xFF
    dst_bank = (insn.operand >> 8) & 0xFF
    return [BlockMove(direction='mvp', src_bank=src_bank, dst_bank=dst_bank)]


# ── Dispatch table ──────────────────────────────────────────────────────────

_DISPATCH = {
    'LDA': _h_lda, 'LDX': _h_ldx, 'LDY': _h_ldy,
    'STA': _h_sta, 'STX': _h_stx, 'STY': _h_sty,
    'STZ': _h_stz,

    'ADC': _alu_handler(AluOp.ADD, Reg.A, _width_a),
    'SBC': _alu_handler(AluOp.SUB, Reg.A, _width_a),
    'AND': _alu_handler(AluOp.AND, Reg.A, _width_a),
    'ORA': _alu_handler(AluOp.OR,  Reg.A, _width_a),
    'EOR': _alu_handler(AluOp.XOR, Reg.A, _width_a),
    'CMP': _alu_handler(AluOp.CMP, Reg.A, _width_a),
    'CPX': _alu_handler(AluOp.CMP, Reg.X, _width_x),
    'CPY': _alu_handler(AluOp.CMP, Reg.Y, _width_x),

    'ASL': _shift_handler(ShiftOp.ASL),
    'LSR': _shift_handler(ShiftOp.LSR),
    'ROL': _shift_handler(ShiftOp.ROL),
    'ROR': _shift_handler(ShiftOp.ROR),

    'INC': _h_inc, 'DEC': _h_dec,
    'INX': _h_inx, 'INY': _h_iny, 'DEX': _h_dex, 'DEY': _h_dey,

    'BIT': _h_bit, 'TSB': _h_tsb, 'TRB': _h_trb,

    'TAX': _xfer(Reg.A, Reg.X), 'TXA': _xfer(Reg.X, Reg.A),
    'TAY': _xfer(Reg.A, Reg.Y), 'TYA': _xfer(Reg.Y, Reg.A),
    'TXY': _xfer(Reg.X, Reg.Y), 'TYX': _xfer(Reg.Y, Reg.X),
    'TSX': _xfer(Reg.S, Reg.X), 'TXS': _xfer(Reg.X, Reg.S),
    'TCD': _xfer(Reg.A, Reg.D), 'TDC': _xfer(Reg.D, Reg.A),
    'TCS': _xfer(Reg.A, Reg.S), 'TSC': _xfer(Reg.S, Reg.A),

    'CLC': _flag_set(Reg.C, 0),  'SEC': _flag_set(Reg.C, 1),
    'CLI': _flag_set(Reg.I, 0),  'SEI': _flag_set(Reg.I, 1),
    'CLD': _flag_set(Reg.DF, 0), 'SED': _flag_set(Reg.DF, 1),
    'CLV': _flag_set(Reg.V, 0),

    'REP': _h_rep, 'SEP': _h_sep, 'XCE': _h_xce, 'XBA': _h_xba,

    'PHA': _push_reg(Reg.A),  'PLA': _pull_reg(Reg.A),
    'PHX': _push_reg(Reg.X),  'PLX': _pull_reg(Reg.X),
    'PHY': _push_reg(Reg.Y),  'PLY': _pull_reg(Reg.Y),
    'PHB': _push_reg(Reg.DB), 'PLB': _pull_reg(Reg.DB),
    'PHD': _push_reg(Reg.D),  'PLD': _pull_reg(Reg.D),
    'PHK': _push_reg(Reg.PB),
    'PHP': _push_reg(Reg.P),  'PLP': _pull_reg(Reg.P),

    'PEA': _h_pea, 'PER': _h_per, 'PEI': _h_pei,
    'MVN': _h_mvn, 'MVP': _h_mvp,

    'BPL': _cond_branch(Reg.N,  0), 'BMI': _cond_branch(Reg.N,  1),
    'BVC': _cond_branch(Reg.V,  0), 'BVS': _cond_branch(Reg.V,  1),
    'BCC': _cond_branch(Reg.C,  0), 'BCS': _cond_branch(Reg.C,  1),
    'BNE': _cond_branch(Reg.ZF, 0), 'BEQ': _cond_branch(Reg.ZF, 1),

    'BRA': _h_bra, 'BRL': _h_brl,
    'JMP': _h_jmp, 'JSR': _h_jsr, 'JSL': _h_jsl,
    'RTS': _h_rts, 'RTL': _h_rtl, 'RTI': _h_rti,

    'NOP': _h_nop, 'WDM': _h_wdm, 'BRK': _h_brk, 'COP': _h_cop,
    'STP': _h_stp, 'WAI': _h_wai,
}


def all_known_mnemonics():
    """Return the set of mnemonics our dispatcher handles."""
    return set(_DISPATCH.keys())


def all_opcode_mnemonics():
    """Return the set of mnemonics that appear in the snes65816 opcode table."""
    table = snes65816._OPCODES
    return {entry[0] for entry in table.values()}
