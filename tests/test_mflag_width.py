"""M-flag width tracking across SEP #$20 transitions at the emitter.

Pins the fix for the HandleSPCUploads_Inner SPC-upload loop symptom:

    REP #$20          ; M -> 0 (A is 16-bit)
    LDA #$AABB
    STA HW_APUIO0     ; 16-bit write — A held as uint16 variable
    SEP #$20          ; M -> 1 (A is 8-bit, but tracked value is still uint16)
  - CMP HW_APUIO0     ; 8-bit compare — must read only A's low byte
    BNE -

Before the fix, SEP #$20 narrowed the hidden B accumulator (high byte) but
left self.A as the uint16 identifier from before SEP. The subsequent 8-bit
CMP then emitted `(v_uint16 - ReadReg(...)) != 0`, which in C promotes to
int and only matches zero when ALL 16 bits match — an 8-bit SPC echo
(low byte only) can never satisfy that, so the poll hangs forever.

The emitter must narrow self.A at SEP #$20 so downstream 8-bit operations
see the low byte only.
"""
import pathlib
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / 'recompiler'))

import recomp  # noqa: E402
from snes65816 import decode_insn  # noqa: E402


def _decode_linear(rom: bytes, start_pc: int, bank: int = 0,
                    m: int = 1, x: int = 1):
    """Decode a linear ROM snippet matching the real decoder's m_flag
    stamping: insn.m_flag = m BEFORE applying REP/SEP."""
    out = []
    off = 0
    pc = start_pc
    while off < len(rom):
        insn = decode_insn(rom, off, pc, bank, m=m, x=x)
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


def _emit_body(rom: bytes, start_pc: int = 0x8000) -> str:
    """Decode + emit a function body, return the C text (header + body)."""
    insns = _decode_linear(rom, start_pc)
    lines = recomp.emit_function(
        name='test_fn', insns=insns, bank=0,
        func_names={}, func_sigs={},
        sig='void()', rom=rom,
    )
    return '\n'.join(lines)


def test_SEP_narrows_A_low_byte_for_subsequent_CMP():
    # REP #$20 ; LDA #$AABB ; STA $2140 ; SEP #$20 ;
    # -: CMP $2140 ; BNE - ; RTS
    # CMP alone emits no C (only sets carry/flag_src); we need the BNE
    # to force the emitter to render the compare expression.
    rom = bytes([
        0xC2, 0x20,              # $8000: REP #$20
        0xA9, 0xBB, 0xAA,        # $8002: LDA #$AABB (16-bit)
        0x8D, 0x40, 0x21,        # $8005: STA $2140 (16-bit)
        0xE2, 0x20,              # $8008: SEP #$20
        0xCD, 0x40, 0x21,        # $800a: CMP $2140 (8-bit after SEP)
        0xD0, 0xFB,              # $800d: BNE $800a (back to CMP)
        0x60,                    # $800f: RTS
    ])
    body = _emit_body(rom)
    # The generated CMP line must not reference the 16-bit A variable raw.
    # The fix introduces an 8-bit narrowing assignment between SEP and CMP,
    # e.g. `vN = (uint8)(vM);`, and the CMP uses vN.
    assert '(uint8)(' in body, (
        'SEP #$20 must narrow A to (uint8) before the subsequent 8-bit '
        'CMP; no narrowing cast found in emitted body:\n' + body
    )
    # Stronger check: find the CMP-derived compare expression and assert
    # the operand is not the pre-SEP uint16 identifier.
    #
    # The emitter creates v1 = 0xAABB before STA, so a raw `v1 - ReadReg(`
    # in the CMP line is the unfixed bug signature.
    import re
    # After the fix we expect a new 8-bit var to be introduced between SEP
    # and CMP. Look for the pattern `vN = (uint8)(v1);` — we assert at
    # LEAST ONE (uint8) narrowing appears before the CMP-derived line.
    cmp_idx = body.find('ReadReg(0x2140)')
    assert cmp_idx != -1, f'no ReadReg(0x2140) in body:\n{body}'
    before_cmp = body[:cmp_idx]
    narrow_re = re.compile(r'=\s*\(uint8\)\(')
    assert narrow_re.search(before_cmp), (
        'expected a `(uint8)(...)` narrowing assignment between SEP and '
        'the CMP read of $2140; none found before CMP in:\n' + body
    )
