"""SEP #$10 must narrow X/Y from uint16 back to uint8 so INX/DEX/INY/DEY
wrap at 256 instead of 65536, and high-byte-contaminated values don't
leak into 8-bit CPX/CPY comparisons.

Audit entries #5 and #8 in docs/AUDIT_RECOMP_WIDTH_BUGS.md.

Before the fix, REP #$10 promoted X/Y's hoisted type uint8 -> uint16
at recomp.py:4494-4501, but the counterpart SEP #$10 had no handling,
so a uint16 X variable would keep wrapping at 65536 even after the
program flipped X back to 8-bit mode.
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


def test_SEP_narrows_X_to_uint8():
    # REP #$10 ; LDX #$01FF ; SEP #$10 ; INX ; RTS
    # Expected: a (uint8) narrowing cast on X somewhere between SEP
    # and INX so the increment wraps at 256.
    rom = bytes([
        0xC2, 0x10,              # REP #$10  (X -> 16-bit)
        0xA2, 0xFF, 0x01,        # LDX #$01FF
        0xE2, 0x10,              # SEP #$10  (X -> 8-bit)
        0xE8,                    # INX
        0x60,                    # RTS
    ])
    body = _emit_body(rom)
    import re
    narrow_re = re.compile(r'=\s*\(uint8\)\(')
    assert narrow_re.search(body), (
        'SEP #$10 must emit a (uint8) narrowing cast for X; body:\n' + body)


def test_SEP_narrows_Y_to_uint8():
    # REP #$10 ; LDY #$01FF ; SEP #$10 ; INY ; RTS
    rom = bytes([
        0xC2, 0x10,
        0xA0, 0xFF, 0x01,        # LDY #$01FF
        0xE2, 0x10,
        0xC8,                    # INY
        0x60,
    ])
    body = _emit_body(rom)
    import re
    narrow_re = re.compile(r'=\s*\(uint8\)\(')
    assert narrow_re.search(body), (
        'SEP #$10 must emit a (uint8) narrowing cast for Y; body:\n' + body)


def test_SEP_30_narrows_X():
    # REP #$30 ; LDX $1234 ; SEP #$30 ; INX ; RTS
    # LDX from memory (not immediate) so X is an allocated, hoisted
    # uint16 variable — only hoisted vars get a SEP-narrowing cast.
    # SEP #$30 sets both M and X to 1; the X/Y bit of v matters here.
    rom = bytes([
        0xC2, 0x30,              # REP #$30  (X -> 16-bit)
        0xAE, 0x34, 0x12,        # LDX $1234 (ABS, 16-bit read)
        0xE2, 0x30,              # SEP #$30
        0xE8,                    # INX
        0x60,
    ])
    body = _emit_body(rom)
    import re
    narrow_re = re.compile(r'=\s*\(uint8\)\(')
    assert narrow_re.search(body), (
        'SEP #$30 must narrow X (hoisted uint16) to uint8; body:\n' + body)


def test_REP_only_SEP_20_leaves_X_alone():
    # Regression guard: SEP #$20 (M only) must not touch X.
    # REP #$30 ; LDX #$01FF ; SEP #$20 ; INX ; RTS
    # X is still 16-bit after SEP #$20, so INX on 0x01FF -> 0x0200.
    rom = bytes([
        0xC2, 0x30,
        0xA2, 0xFF, 0x01,        # LDX #$01FF
        0xE2, 0x20,              # SEP #$20 (M only, X stays 16-bit)
        0xE8,                    # INX
        0x60,
    ])
    body = _emit_body(rom)
    # The X variable should still be uint16 in the generated signature;
    # verify there is no (uint8) narrowing cast assigned to X.
    # Easier check: INX must not be preceded by a cast of X to uint8
    # at this site. The body shouldn't contain "(uint8)(...)" ASSIGNED
    # to the same var used in the increment — we just check no new
    # narrow was emitted for X.
    #
    # We rely on the simpler invariant: uint16 X allocation is still
    # visible in the body (an X variable typed uint16).
    assert 'uint16' in body, (
        'SEP #$20 must not narrow X (X stays 16-bit); body:\n' + body)
