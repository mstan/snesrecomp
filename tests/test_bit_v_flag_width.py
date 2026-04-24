"""BIT V-flag must be extracted from bit 14 of memory when M=0,
bit 6 when M=1. Audit entry #4 in docs/AUDIT_RECOMP_WIDTH_BUGS.md.

Prior bug: `self.overflow = f'({mem}) & 0x40'` unconditionally, so
a 16-bit BIT-then-BVS/BVC branched on the wrong bit.
"""
import pathlib
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / 'recompiler'))

import recomp  # noqa: E402
from snes65816 import decode_insn  # noqa: E402


def _decode_linear(rom: bytes, start_pc: int = 0x8000, m: int = 1, x: int = 1):
    out = []
    off = 0
    pc = start_pc
    while off < len(rom):
        insn = decode_insn(rom, off, pc, 0, m=m, x=x)
        if insn is None:
            break
        insn.m_flag = m
        insn.x_flag = x
        out.append(insn)
        if insn.mnem == 'REP':
            if insn.operand & 0x20: m = 0
            if insn.operand & 0x10: x = 0
        elif insn.mnem == 'SEP':
            if insn.operand & 0x20: m = 1
            if insn.operand & 0x10: x = 1
        off += insn.length
        pc = (pc + insn.length) & 0xFFFF
        if insn.mnem in ('RTS', 'RTL', 'RTI'):
            break
    return out


def _emit_body(rom: bytes) -> str:
    insns = _decode_linear(rom)
    lines = recomp.emit_function(
        name='test_fn', insns=insns, bank=0,
        func_names={}, func_sigs={},
        sig='void()', rom=rom,
    )
    return '\n'.join(lines)


def test_BIT_ABS_16bit_V_from_bit_14():
    # REP #$20 ; BIT $1234 ; BVS +2 ; RTS
    # BVS forces the emitter to reference self.overflow.
    rom = bytes([
        0xC2, 0x20,              # REP #$20
        0x2C, 0x34, 0x12,        # BIT $1234 (ABS, 16-bit)
        0x70, 0x02,              # BVS +2
        0x60, 0x60, 0x60,        # pad + RTS
    ])
    body = _emit_body(rom)
    assert '0x4000' in body, (
        '16-bit BIT ABS must set V from bit 14 (0x4000); body:\n' + body)
    assert '& 0x40)' not in body and '& 0x40 ' not in body, (
        '16-bit BIT must not use bit-6 mask:\n' + body)


def test_BIT_DP_16bit_V_from_bit_14():
    rom = bytes([
        0xC2, 0x20,              # REP #$20
        0x24, 0x00,              # BIT $00 (DP, 16-bit)
        0x70, 0x02,              # BVS
        0x60, 0x60, 0x60,
    ])
    body = _emit_body(rom)
    assert '0x4000' in body, (
        '16-bit BIT DP must set V from bit 14; body:\n' + body)


def test_BIT_ABS_8bit_V_from_bit_6():
    # M=1 default — regression guard
    rom = bytes([
        0x2C, 0x34, 0x12,        # BIT $1234 (ABS, 8-bit)
        0x70, 0x02,              # BVS
        0x60, 0x60, 0x60,
    ])
    body = _emit_body(rom)
    assert '0x40' in body, (
        '8-bit BIT ABS must set V from bit 6; body:\n' + body)
    assert '0x4000' not in body, (
        '8-bit BIT must not use bit-14 mask:\n' + body)


def test_BIT_IMM_does_not_set_V():
    # BIT #imm sets Z only (no V, no N) — regression guard that the
    # immediate-mode path does not set self.overflow.
    rom = bytes([
        0xC2, 0x20,              # REP #$20
        0x89, 0x00, 0x80,        # BIT #$8000 (IMM, 16-bit)
        0x70, 0x02,              # BVS
        0x60, 0x60, 0x60,
    ])
    body = _emit_body(rom)
    # Either no overflow expression, or one left from before — but
    # the BIT #imm site must not freshly set it to `(0x8000) & 0x4000`.
    # Easiest check: the BVS branch should emit `/* overflow? */ 0`
    # or reference a prior flag, not the immediate value.
    assert '0x8000) & 0x4000' not in body and '(0x8000) & 0x40' not in body, (
        'BIT #imm must NOT set V flag:\n' + body)
