"""`(dp)` / `(dp),Y` / `(dp,X)` must resolve through the data bank.

The 16-bit-indirect addressing modes form the effective address from
the 2-byte pointer at DP combined with DB — not DP + WRAM base.
When DB is a ROM bank (common: bank 00 with no PHK/PLB changes),
writing `g_ram[ptr_lo | ptr_hi<<8]` silently reads WRAM bytes where
the ROM should be, which is how SMW's `BufferPalettesRoutines_LoadColors`
ended up reading 0xfc for a palette byte where oracle read 0x49.

The emitter now uses IndirPtrDB(), a runtime helper that combines
DP[0..1] with g_cpu->db and dispatches to ROM/WRAM correctly.
"""
import pathlib
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / 'recompiler'))

import recomp  # noqa: E402
from snes65816 import decode_insn  # noqa: E402


def _decode_linear(rom: bytes, start_pc: int, bank: int = 0,
                    m: int = 1, x: int = 1):
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


def _emit(rom: bytes, start: int = 0x8000, end: int = None,
          sig: str = 'void()') -> str:
    insns = _decode_linear(rom, start)
    if end is None:
        end = (insns[-1].addr & 0xFFFF) + insns[-1].length
    lines = recomp.emit_function(
        name='test_fn', insns=insns, bank=0,
        func_names={}, func_sigs={},
        sig=sig, rom=rom, end_addr=end,
    )
    return '\n'.join(lines)


def test_LDA_indirect_y_16bit_uses_IndirPtrDB():
    # REP #$20 ; LDA ($00),Y ; STA $0500 ; RTS  — wide read through (dp),Y.
    # Opcodes: C2 20, B1 00, 8D 00 05, 60
    rom = bytes([
        0xC2, 0x20,
        0xB1, 0x00,
        0x8D, 0x00, 0x05,
        0x60,
    ])
    body = _emit(rom)
    assert 'IndirPtrDB(' in body, (
        f'expected IndirPtrDB in emit for (dp),Y wide read:\n{body}'
    )
    assert 'g_ram + (g_ram[0x' not in body, (
        f'old g_ram-relative emit must not reappear:\n{body}'
    )


def test_STA_indirect_y_uses_IndirPtrDB():
    # LDY #$00 ; LDA #$42 ; STA ($00),Y ; RTS
    rom = bytes([
        0xA0, 0x00,
        0xA9, 0x42,
        0x91, 0x00,
        0x60,
    ])
    body = _emit(rom)
    assert 'IndirPtrDB(' in body, (
        f'expected IndirPtrDB in emit for (dp),Y store:\n{body}'
    )


def test_LDA_dp_indirect_no_index_uses_IndirPtrDB():
    # LDA ($00) ; STA $0500 ; RTS  — observe the load by storing it,
    # otherwise dead-store elimination skips the LDA emit entirely.
    # Opcodes: B2 00, 8D 00 05, 60.
    rom = bytes([
        0xB2, 0x00,
        0x8D, 0x00, 0x05,
        0x60,
    ])
    body = _emit(rom)
    assert 'IndirPtrDB(' in body, (
        f'expected IndirPtrDB in emit for (dp):\n{body}'
    )
