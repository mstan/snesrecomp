"""ASL / LSR / ROL / ROR on WRAM memory operands must be word-width
when M=0 (a_type=uint16).

Before the fix, every memory variant of these four shift/rotate
opcodes unconditionally routed through `_emit_rmw8`, narrowing a
16-bit RMW to 8 bits regardless of M. This silently dropped the
high byte of the target word and extracted the carry from the
wrong bit.

Audit entry #2 in docs/AUDIT_RECOMP_WIDTH_BUGS.md.
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


def test_ASL_DP_16bit_uses_word_RMW():
    # REP #$20 ; ASL $00 ; RTS
    rom = bytes([
        0xC2, 0x20,        # REP #$20 (M -> 0)
        0x06, 0x00,        # ASL $00  (direct page)
        0x60,              # RTS
    ])
    body = _emit_body(rom)
    # 16-bit read and 16-bit write must be visible; carry from bit 15.
    assert ('GET_WORD(g_ram + 0x0)' in body or 'RDB_LOAD16(0x0)' in body), (
        '16-bit ASL DP must read a word; body:\n' + body)
    assert '>> 15' in body, (
        '16-bit ASL DP must extract carry from bit 15; body:\n' + body)
    # The store side must be 16-bit too.
    assert ('*(uint16*)' in body or 'RDB_STORE16(' in body
            or 'WORD(g_ram + 0x0)' in body), (
        '16-bit ASL DP must write a word, not a byte; body:\n' + body)


def test_LSR_DP_16bit_uses_word_RMW():
    rom = bytes([
        0xC2, 0x20,
        0x46, 0x00,        # LSR $00
        0x60,
    ])
    body = _emit_body(rom)
    assert ('GET_WORD(g_ram + 0x0)' in body or 'RDB_LOAD16(0x0)' in body), (
        '16-bit LSR DP must read a word; body:\n' + body)
    # LSR carry is bit 0 (same for both widths) — but the shift itself
    # must be on uint16 so the high byte is preserved/shifted down.
    # Check: no `(uint8)(... >> 1)` narrowing sneaking in.
    assert ('*(uint16*)' in body or 'RDB_STORE16(' in body), (
        '16-bit LSR DP must write a word; body:\n' + body)


def test_ROL_DP_16bit_uses_word_RMW():
    rom = bytes([
        0x38,              # SEC (carry-in = 1)
        0xC2, 0x20,
        0x26, 0x00,        # ROL $00
        0x60,
    ])
    body = _emit_body(rom)
    assert ('GET_WORD(g_ram + 0x0)' in body or 'RDB_LOAD16(0x0)' in body), (
        '16-bit ROL DP must read a word; body:\n' + body)
    assert '>> 15' in body, (
        '16-bit ROL DP must extract carry from bit 15; body:\n' + body)
    assert ('*(uint16*)' in body or 'RDB_STORE16(' in body), (
        '16-bit ROL DP must write a word; body:\n' + body)


def test_ROR_DP_16bit_uses_word_RMW():
    rom = bytes([
        0x38,              # SEC (carry-in = 1, will inject into bit 15)
        0xC2, 0x20,
        0x66, 0x00,        # ROR $00
        0x60,
    ])
    body = _emit_body(rom)
    assert ('GET_WORD(g_ram + 0x0)' in body or 'RDB_LOAD16(0x0)' in body), (
        '16-bit ROR DP must read a word; body:\n' + body)
    # Carry-in injected at bit 15 of the WORD, not bit 7.
    assert '<< 15' in body, (
        '16-bit ROR DP must inject carry at bit 15; body:\n' + body)
    assert ('*(uint16*)' in body or 'RDB_STORE16(' in body), (
        '16-bit ROR DP must write a word; body:\n' + body)


def test_ASL_ABS_16bit_uses_word_RMW():
    # REP #$20 ; ASL $1234 ; RTS
    rom = bytes([
        0xC2, 0x20,
        0x0E, 0x34, 0x12,  # ASL $1234
        0x60,
    ])
    body = _emit_body(rom)
    assert ('GET_WORD(g_ram + 0x1234)' in body or 'RDB_LOAD16(0x1234)' in body), (
        '16-bit ASL ABS must read a word; body:\n' + body)
    assert '>> 15' in body, body


def test_ASL_DP_8bit_still_uses_byte_RMW():
    # Regression guard: M=1 (default) path must remain byte-width.
    rom = bytes([
        0x06, 0x00,        # ASL $00  (M=1, byte)
        0x60,
    ])
    body = _emit_body(rom)
    # No word accesses should appear in the ASL output.
    assert '>> 15' not in body, (
        '8-bit ASL DP must NOT extract carry from bit 15:\n' + body)
    assert ('>> 7' in body), (
        '8-bit ASL DP must extract carry from bit 7:\n' + body)


def test_ROR_DP_8bit_still_uses_byte_RMW():
    rom = bytes([
        0x38,              # SEC
        0x66, 0x00,        # ROR $00  (M=1, byte)
        0x60,
    ])
    body = _emit_body(rom)
    assert '<< 15' not in body, (
        '8-bit ROR DP must NOT inject carry at bit 15:\n' + body)
    assert '<< 7' in body, (
        '8-bit ROR DP must inject carry at bit 7:\n' + body)
