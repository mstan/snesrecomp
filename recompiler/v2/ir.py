"""snesrecomp.recompiler.v2.ir

Stateful IR for the v2 pipeline. IR ops are typed transformations on a
CpuState (A, B, X, Y, S, D, DB, PB, P, M flag, X flag, emulation flag).
Each 65816 instruction lowers to a list of IR ops via `lowering.py`.

Replaces v1's emit-time C-expression-string tracking
(EmitCtx.A/B/X/Y/carry/flag_src etc., recomp.py:2829-2865) and the
heuristic-phi machinery layered on top (_branch_states, _label_a/b/x/y,
_emit_backedge_phi, _emit_branch, _ensure_mutable_x). In v2, register
values are IR `Value` handles produced by IR ops; merges across joins
are real phi nodes on the IR, not C-string string manipulation.

This module defines:
- `Reg` — symbolic register identifier (A/B/X/Y/S/D/DB/PB/M_FLAG/X_FLAG/E_FLAG plus
  individual P bits N, V, Z, C, I, D_FLAG).
- `SegRef` — resolved memory reference (direct page, abs+DB, long, stack, indirect, etc.).
- `Value` — opaque handle produced by an IR op; downstream consumers identify the
  producer by the (block, op_index) it came from.
- `IROp` and a closed set of op subclasses.

The IR is intentionally low-level and explicit. Optimization passes
(SSA construction, dead-code, peephole) live in later phases on top of
this — Phase 8 deferred per the plan.
"""

from dataclasses import dataclass, field
from enum import Enum
from typing import List, Optional, Tuple


# ── Registers ────────────────────────────────────────────────────────────────

class Reg(Enum):
    """Symbolic 65816 CPU register / flag identifier."""
    A = 'A'
    B = 'B'
    X = 'X'
    Y = 'Y'
    S = 'S'
    D = 'D'
    DB = 'DB'
    PB = 'PB'
    P = 'P'           # full P byte
    # Individual P-flag bits, exposed as their own slots so IR ops don't
    # need to mask P every time:
    M = 'M'           # accumulator/memory width: 1=8-bit, 0=16-bit
    XF = 'XF'         # index width: 1=8-bit, 0=16-bit
    E = 'E'           # emulation flag (XCE)
    N = 'N'
    V = 'V'
    ZF = 'ZF'         # 'Z' clashes with stylistic uses; use ZF for the flag
    C = 'C'
    I = 'I'           # interrupt-disable
    DF = 'DF'         # decimal mode


# ── Memory references ───────────────────────────────────────────────────────

class SegKind(Enum):
    """How an addressing mode resolves at runtime."""
    DIRECT = 'direct'              # D + dp_offset (+X / +Y if indexed)
    ABS_BANK = 'abs_bank'          # DB << 16 | abs (+X / +Y if indexed)
    LONG = 'long'                  # bank << 16 | abs (+X if indexed)
    STACK = 'stack'                # S + offset (or stack-relative-indirect-Y composed)
    DP_INDIRECT = 'dp_indirect'    # ((D + dp) word) (+Y if indirect-Y), data-bank
    DP_INDIRECT_LONG = 'dp_indirect_long'  # ((D + dp) long) (+Y)
    ABS_INDIRECT = 'abs_indirect'  # ((PB:abs)) — indirect JMP, PB-bank
    ABS_INDIRECT_LONG = 'abs_indirect_long'  # ((abs)) long indirect
    ABS_INDIRECT_X = 'abs_indirect_x'  # ((PB:abs+X)) — indirect-X JMP/JSR
    DP_INDIRECT_X = 'dp_indirect_x'    # ((D + dp + X)) data-bank
    STACK_REL_INDIRECT_Y = 'stack_rel_indirect_y'  # ((S + offs)) + Y, data-bank


@dataclass(frozen=True)
class SegRef:
    """Resolved memory reference for one IR Read/Write.

    Components depend on `kind`. Unused fields stay None / 0.
    """
    kind: SegKind
    offset: int = 0           # immediate component (dp byte, abs word, long 24-bit)
    bank: Optional[int] = None  # for LONG kind (else taken from CpuState.DB / .PB at runtime)
    index: Optional[Reg] = None  # Reg.X or Reg.Y if indexed; else None


# ── Values ──────────────────────────────────────────────────────────────────

@dataclass(frozen=True)
class Value:
    """An opaque handle for an IR-produced value.

    `vid` is unique within a function; assigned by the lowering driver.
    Width (8 / 16) is intrinsic to the producing op and is not stored
    here — consumers either know from the op or read the producer's
    metadata.
    """
    vid: int


# ── IR ops ──────────────────────────────────────────────────────────────────

@dataclass(frozen=True)
class IROp:
    """Base — every concrete op is a frozen dataclass subclass below.

    Each op may produce a `out: Value` (None if the op writes only to
    CpuState slots / memory).
    """
    pass


# Memory access
@dataclass(frozen=True)
class Read(IROp):
    seg: SegRef
    width: int               # 1 or 2 bytes
    out: Value


@dataclass(frozen=True)
class Write(IROp):
    seg: SegRef
    src: Value
    width: int


# Register reads / writes (CpuState slots)
@dataclass(frozen=True)
class ReadReg(IROp):
    reg: Reg
    out: Value


@dataclass(frozen=True)
class WriteReg(IROp):
    reg: Reg
    src: Value


# Constants
@dataclass(frozen=True)
class ConstI(IROp):
    value: int
    width: int                 # 1 or 2
    out: Value


# ALU
class AluOp(Enum):
    ADD = 'add'   # with carry
    SUB = 'sub'   # with borrow
    AND = 'and'
    OR = 'or'
    XOR = 'xor'
    CMP = 'cmp'   # sub for flags only, no destination


@dataclass(frozen=True)
class Alu(IROp):
    op: AluOp
    lhs: Value
    rhs: Value
    width: int                 # 1 or 2 (resolved from M at lowering)
    out: Optional[Value]       # None for CMP


# Shifts / rotates
class ShiftOp(Enum):
    ASL = 'asl'
    LSR = 'lsr'
    ROL = 'rol'
    ROR = 'ror'


@dataclass(frozen=True)
class Shift(IROp):
    op: ShiftOp
    src: Value
    width: int
    out: Value
    # Updates C flag implicitly per the shift kind.


# Increment / decrement of a register
@dataclass(frozen=True)
class IncReg(IROp):
    reg: Reg
    delta: int                 # +1 / -1 (INX/DEX/INY/DEY/INC A/DEC A)


# Bit test / set / clear (flags only or memory write)
@dataclass(frozen=True)
class BitTest(IROp):
    """BIT — sets N V Z based on operand & A."""
    operand: Value
    width: int


@dataclass(frozen=True)
class BitSetMem(IROp):
    """TSB — set bits in memory: mem |= A; flag Z on (mem & A) == 0 PRE-write."""
    seg: SegRef
    width: int


@dataclass(frozen=True)
class BitClearMem(IROp):
    """TRB — clear bits in memory: mem &= ~A; flag Z on (mem & A) == 0 PRE-write."""
    seg: SegRef
    width: int


# Flag / mode ops
@dataclass(frozen=True)
class SetFlag(IROp):
    flag: Reg
    value: int                 # 0 or 1


@dataclass(frozen=True)
class RepFlags(IROp):
    """REP #imm — clear bits set in `mask`. Updates M, XF, plus other P bits if requested."""
    mask: int


@dataclass(frozen=True)
class SepFlags(IROp):
    """SEP #imm — set bits set in `mask`."""
    mask: int


@dataclass(frozen=True)
class XCE(IROp):
    """Exchange C and emulation flag."""
    pass


# Stack ops
@dataclass(frozen=True)
class Push(IROp):
    src: Value
    width: int


@dataclass(frozen=True)
class Pull(IROp):
    width: int
    out: Value


@dataclass(frozen=True)
class PushReg(IROp):
    """PHA / PHX / PHY / PHB / PHD / PHK / PHP."""
    reg: Reg


@dataclass(frozen=True)
class PullReg(IROp):
    """PLA / PLX / PLY / PLB / PLD / PLP."""
    reg: Reg


# Block move
@dataclass(frozen=True)
class BlockMove(IROp):
    """MVN / MVP — A is the (count - 1); X is src offset, Y is dst offset.
    `src_bank`, `dst_bank` are the immediate operands."""
    direction: str             # 'mvn' (incrementing) | 'mvp' (decrementing)
    src_bank: int
    dst_bank: int


# Control flow
@dataclass(frozen=True)
class CondBranch(IROp):
    """Conditional branch on a P-flag bit. Emitted at the end of a block.

    `flag` is one of N / V / ZF / C; `take_if` is 0 or 1 (e.g. BEQ -> ZF==1).
    Successor block keys are encoded by the surrounding block's CFG edges,
    not stored in the op itself.
    """
    flag: Reg
    take_if: int


@dataclass(frozen=True)
class Goto(IROp):
    """Unconditional control transfer (BRA, BRL, JMP ABS, fall-through).
    The target is encoded by the surrounding block's single successor."""
    pass


@dataclass(frozen=True)
class IndirectGoto(IROp):
    """Indirect JMP — successor list comes from the cfg (or is a stub if
    the dispatch table isn't statically known)."""
    seg: SegRef


@dataclass(frozen=True)
class Call(IROp):
    """JSR ABS / JSR (abs,X) / JSL — pushes return + transfers to target.

    `target` is the resolved 24-bit address for ABS and LONG; None for
    indirect-X dispatch where the surrounding cfg supplies the table."""
    target: Optional[int]
    long: bool                 # True for JSL (24-bit return), False for JSR
    indirect: bool = False     # True for JSR (abs,X)


@dataclass(frozen=True)
class Return(IROp):
    """RTS / RTL / RTI."""
    long: bool                 # True for RTL/RTI, False for RTS
    interrupt: bool = False    # True for RTI


# Misc
@dataclass(frozen=True)
class Transfer(IROp):
    """TAX / TXA / TAY / TYA / TXY / TYX / TSX / TXS / TCD / TDC / TCS / TSC."""
    src: Reg
    dst: Reg


@dataclass(frozen=True)
class XBA(IROp):
    """Exchange B and A halves of the 16-bit accumulator."""
    pass


@dataclass(frozen=True)
class Nop(IROp):
    """NOP / WDM / undecoded-but-permitted."""
    pass


@dataclass(frozen=True)
class Break(IROp):
    """BRK / COP — software interrupt."""
    cop: bool


@dataclass(frozen=True)
class Stop(IROp):
    """STP / WAI — halt."""
    wait: bool                 # True for WAI; False for STP


# PEI / PEA / PER — push effective address-class
@dataclass(frozen=True)
class PushEffectiveAddress(IROp):
    """PEA #abs / PER label / PEI (dp).
    `seg` resolves the source; pushes a 16-bit value."""
    seg: SegRef


# Container — a sequence of IR ops for one block
@dataclass
class IRBlock:
    """An ordered sequence of IR ops corresponding to one V2Block."""
    ops: List[IROp] = field(default_factory=list)
