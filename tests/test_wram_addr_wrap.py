"""Indexed WRAM accesses must wrap the effective address at 16 bits
so X=0xFFFF on DP_X (or similar) reads from bank-0 WRAM and not from
bank \$7F (Memory.RAM[0x10000+]).

Phase B fuzz (2026-04-24) caught this on INC DP_X / DEC DP_X / INC
ABS_X / DEC ABS_X with X seeded to 0xFFFF or 0x7FFF. This test pins
the mask at the codegen level.
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


def test_INC_DP_X_uses_uint16_mask_on_effective_addr():
    # REP #$30 ; LDX #$FFFF ; INC $10,X ; RTS
    rom = bytes([
        0xC2, 0x30,              # REP #$30
        0xA2, 0xFF, 0xFF,        # LDX #$FFFF
        0xF6, 0x10,              # INC $10,X  (DP_X)
        0x60,                    # RTS
    ])
    body = _emit_body(rom)
    # Indexed access must apply (uint16)(...) mask so X=0xFFFF wraps.
    assert '(uint16)(0x10 +' in body, (
        'INC DP_X must wrap effective addr at 16 bits; body:\n' + body)


def test_LDA_DP_X_uses_uint16_mask():
    # REP #$30 ; LDX #$FFFF ; LDA $10,X ; STA $1F00 ; RTS
    # STA forces the emitter to materialize the LDA expression, so
    # the DP_X read actually appears in the body.
    rom = bytes([
        0xC2, 0x30,
        0xA2, 0xFF, 0xFF,
        0xB5, 0x10,              # LDA $10,X  (DP_X)
        0x8D, 0x00, 0x1F,        # STA $1F00  (forces materialization)
        0x60,
    ])
    body = _emit_body(rom)
    assert '(uint16)(0x10 +' in body, (
        'LDA DP_X must wrap effective addr at 16 bits; body:\n' + body)


def test_STA_ABS_X_uses_uint16_mask():
    # REP #$30 ; LDA #$1234 ; LDX #$FFFF ; STA $0100,X ; RTS
    rom = bytes([
        0xC2, 0x30,
        0xA9, 0x34, 0x12,        # LDA #$1234
        0xA2, 0xFF, 0xFF,        # LDX #$FFFF
        0x9D, 0x00, 0x01,        # STA $0100,X  (ABS_X)
        0x60,
    ])
    body = _emit_body(rom)
    assert '(uint16)(0x100 +' in body, (
        'STA ABS_X must wrap effective addr at 16 bits; body:\n' + body)


def test_non_indexed_access_has_no_mask():
    # Regression guard: plain DP / ABS writes must NOT gain a cast
    # (preserves byte-for-byte codegen for the common case).
    rom = bytes([
        0xA5, 0x10,              # LDA $10 (DP)
        0x60,
    ])
    body = _emit_body(rom)
    # LDA $10 without index — base expression should be 0x10 without
    # a uint16 cast.
    assert '(uint16)(0x10 +' not in body, (
        'non-indexed DP access must not mask:\n' + body)
