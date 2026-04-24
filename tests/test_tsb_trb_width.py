"""TSB / TRB on WRAM memory operands must be word-width when M=0.

Audit entry #3 in docs/AUDIT_RECOMP_WIDTH_BUGS.md. Mirror of #2
(shift/rotate memory width) — before the fix, TSB/TRB always
called _emit_rmw8, narrowing a 16-bit test-and-set/reset to a
byte and leaving the high byte at addr+1 untouched.
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


def test_TSB_DP_16bit_uses_word_RMW():
    # REP #$20 ; LDA #$1234 ; TSB $00 ; RTS
    rom = bytes([
        0xC2, 0x20,              # REP #$20 (M -> 0)
        0xA9, 0x34, 0x12,        # LDA #$1234 (A <- 0x1234)
        0x04, 0x00,              # TSB $00  (DP)
        0x60,                    # RTS
    ])
    body = _emit_body(rom)
    assert ('GET_WORD(g_ram + 0x0)' in body or 'RDB_LOAD16(0x0)' in body), (
        '16-bit TSB DP must read a word; body:\n' + body)
    assert ('*(uint16*)' in body or 'RDB_STORE16(' in body), (
        '16-bit TSB DP must write a word, not a byte; body:\n' + body)


def test_TRB_DP_16bit_uses_word_RMW():
    rom = bytes([
        0xC2, 0x20,
        0xA9, 0x34, 0x12,
        0x14, 0x00,              # TRB $00
        0x60,
    ])
    body = _emit_body(rom)
    assert ('GET_WORD(g_ram + 0x0)' in body or 'RDB_LOAD16(0x0)' in body), (
        '16-bit TRB DP must read a word; body:\n' + body)
    assert ('*(uint16*)' in body or 'RDB_STORE16(' in body), (
        '16-bit TRB DP must write a word; body:\n' + body)


def test_TSB_ABS_16bit_uses_word_RMW():
    rom = bytes([
        0xC2, 0x20,
        0xA9, 0x34, 0x12,
        0x0C, 0x00, 0x10,        # TSB $1000  (ABS)
        0x60,
    ])
    body = _emit_body(rom)
    assert ('GET_WORD(g_ram + 0x1000)' in body or 'RDB_LOAD16(0x1000)' in body), (
        '16-bit TSB ABS must read a word; body:\n' + body)
    assert ('*(uint16*)' in body or 'RDB_STORE16(' in body), (
        '16-bit TSB ABS must write a word; body:\n' + body)


def test_TRB_ABS_16bit_uses_word_RMW():
    rom = bytes([
        0xC2, 0x20,
        0xA9, 0x34, 0x12,
        0x1C, 0x00, 0x10,        # TRB $1000
        0x60,
    ])
    body = _emit_body(rom)
    assert ('GET_WORD(g_ram + 0x1000)' in body or 'RDB_LOAD16(0x1000)' in body), (
        '16-bit TRB ABS must read a word; body:\n' + body)
    assert ('*(uint16*)' in body or 'RDB_STORE16(' in body), (
        '16-bit TRB ABS must write a word; body:\n' + body)


def test_TSB_DP_8bit_still_uses_byte_RMW():
    # M=1 (default) — regression guard
    rom = bytes([
        0xA9, 0x80,              # LDA #$80
        0x04, 0x00,              # TSB $00
        0x60,
    ])
    body = _emit_body(rom)
    # No 16-bit WRAM read/write should appear
    assert 'RDB_LOAD16(0x0)' not in body, (
        '8-bit TSB DP must not read a word:\n' + body)
    assert '*(uint16*)' not in body, (
        '8-bit TSB DP must not write a word:\n' + body)


def test_TRB_DP_8bit_still_uses_byte_RMW():
    rom = bytes([
        0xA9, 0x80,
        0x14, 0x00,              # TRB $00
        0x60,
    ])
    body = _emit_body(rom)
    assert 'RDB_LOAD16(0x0)' not in body, (
        '8-bit TRB DP must not read a word:\n' + body)
    assert '*(uint16*)' not in body, (
        '8-bit TRB DP must not write a word:\n' + body)
