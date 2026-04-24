"""ASL / ROL / ROR on the 16-bit accumulator must extract/inject the
carry at bit 15, not bit 7.

Pins the fix for the CODE_00F636 (MarioGFXDMA tile-pointer setup) bug:

    REP #$20          ; M -> 0 (A is 16-bit)
    LDA $09           ; read 16-bit _9
    AND #$F700
    ROR A             ; 16-bit ROR — carry goes INTO bit 15, carry OUT from bit 0
    LSR A
    ADC #$2000
    STA DynGfxTilePtr

Before the fix, the emitter used `(carry_in << 7)` for both 8-bit and
16-bit ROR, injecting the previous carry into bit 7 instead of bit 15
of the 16-bit accumulator. The resulting DynGfxTilePtr was offset by
roughly 0x4000 from the correct value, causing MarioGFXDMA to copy
wrong ROM bytes into the Mario OBJ-tile VRAM region (visible symptom:
Mario's jump/death sprite renders as big-Mario-lower-half doubled).

Same bug also affected ASL A and ROL A on 16-bit A — the carry OUT was
extracted from bit 7 instead of bit 15. Mirror test below.
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


def test_ROR_ACC_16bit_injects_carry_at_bit15():
    # REP #$20 ; LDA #$1234 ; ROR A ; RTS
    rom = bytes([
        0xC2, 0x20,              # REP #$20 (M -> 0)
        0xA9, 0x34, 0x12,        # LDA #$1234
        0x6A,                    # ROR A
        0x60,                    # RTS
    ])
    body = _emit_body(rom)
    # Bit-15 injection expected; bit-7 injection is the bug.
    assert '<< 15' in body, (
        '16-bit ROR A must inject carry into bit 15; emitted body '
        'has no `<< 15`:\n' + body
    )
    assert '<< 7)' not in body, (
        '16-bit ROR A must NOT inject carry into bit 7 (that was the '
        'old uint8-formula bug leaking into the uint16 path):\n' + body
    )


def test_ROL_ACC_16bit_extracts_carry_from_bit15():
    # REP #$20 ; LDA #$8000 ; ROL A ; RTS
    rom = bytes([
        0xC2, 0x20,
        0xA9, 0x00, 0x80,        # LDA #$8000
        0x2A,                    # ROL A
        0x60,                    # RTS
    ])
    body = _emit_body(rom)
    assert '>> 15' in body, (
        '16-bit ROL A must extract carry from bit 15 (the high bit):\n'
        + body
    )
    assert '>> 7)' not in body, (
        '16-bit ROL A must NOT extract carry from bit 7:\n' + body
    )


def test_ASL_ACC_16bit_extracts_carry_from_bit15():
    # REP #$20 ; LDA #$8000 ; ASL A ; RTS
    rom = bytes([
        0xC2, 0x20,
        0xA9, 0x00, 0x80,        # LDA #$8000
        0x0A,                    # ASL A
        0x60,                    # RTS
    ])
    body = _emit_body(rom)
    assert '>> 15' in body, (
        '16-bit ASL A must extract carry from bit 15 (the high bit):\n'
        + body
    )
    assert '>> 7)' not in body, (
        '16-bit ASL A must NOT extract carry from bit 7:\n' + body
    )


def test_ROR_ACC_8bit_still_uses_bit7():
    # Regression guard: make sure the 8-bit path still uses bit 7.
    # Default M=1 at entry; no REP.
    rom = bytes([
        0xA9, 0x34,              # LDA #$34 (8-bit)
        0x6A,                    # ROR A
        0x60,                    # RTS
    ])
    body = _emit_body(rom)
    assert '<< 7' in body, (
        '8-bit ROR A must still inject carry into bit 7; emitted body '
        'has no `<< 7`:\n' + body
    )


def test_ASL_ACC_8bit_still_uses_bit7():
    rom = bytes([
        0xA9, 0x80,              # LDA #$80 (8-bit)
        0x0A,                    # ASL A
        0x60,                    # RTS
    ])
    body = _emit_body(rom)
    assert '>> 7' in body, (
        '8-bit ASL A must still extract carry from bit 7; emitted body '
        'has no `>> 7`:\n' + body
    )
