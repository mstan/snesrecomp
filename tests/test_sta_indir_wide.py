"""STA [dp] / STA [dp],Y in M=0 must emit a 16-bit word store.

Reproduces the SMW `GraphicsDecompressionRoutines_DecompressGFX32And33`
bug at ROM $00:B8AD-$B8D6. The routine does a 3bpp→4bpp conversion
loop:

  REP #$30                     ; A, X, Y -> 16
  ...
- LDA.L MarioGraphics,X        ; 16-bit load
  AND.W #$00FF                 ; keep low byte only
  STA.B [GraphicsUncompPtr]    ; STA word through 24-bit DP pointer
  DEX
  DEC.B GraphicsUncompPtr      ; pointer -= 2 (DEC dp in M=0 = word DEC)
  DEC.B GraphicsUncompPtr
  DEY
  BNE -

The `STA [dp]` in M=0 is a 16-bit word store; it must write both the
low byte at `[dp]` and the high byte at `[dp+1]`. Previously the emitter
called `IndirWriteByte(...)` — an 8-bit store — dropping the high byte.

For the conversion loop, that turned into: bitplane 1 of every written
tile-pair never got populated. The $7E:7D00 AnimatedTiles buffer ended
up with missing bp1 data, which made ground tiles (Yoshi's Island 1
ExAnimation) fail to render.

Root cause: `_indir_write` had no wide path; `_emit_sta16` for
`INDIR_L` / `INDIR_LY` called `_indir_write` the same as the 8-bit
sibling. Fix: new `IndirWriteWord` runtime helper + wide flag threaded
through `_indir_write` + `_emit_sta16` uses it for indirect-long modes
and uses `*(uint16*)(IndirPtrDB(...))` for DB-composed indirect modes.

This test pins the fix against future regressions.
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


def _emit(rom: bytes, start: int = 0x8000, sig: str = 'void()') -> str:
    insns = _decode_linear(rom, start)
    end = (insns[-1].addr & 0xFFFF) + insns[-1].length
    lines = recomp.emit_function(
        name='test_fn', insns=insns, bank=0,
        func_names={}, func_sigs={},
        sig=sig, rom=rom, end_addr=end,
    )
    return '\n'.join(lines)


def test_STA_indir_long_in_A16_emits_word_store():
    # REP #$30 (A,X,Y -> 16) ; LDA #$abcd ; STA [$8d] ; RTS
    rom = bytes([
        0xC2, 0x30,         # REP #$30
        0xA9, 0xcd, 0xab,   # LDA #$abcd
        0x87, 0x8d,         # STA [$8d]
        0x60,               # RTS
    ])
    body = _emit(rom)
    assert 'IndirWriteByte' not in body, (
        'STA [dp] in M=0 must NOT emit IndirWriteByte (drops high byte). '
        f'Body:\n{body}'
    )
    assert 'IndirWriteWord' in body, (
        f'STA [dp] in M=0 must emit IndirWriteWord. Body:\n{body}'
    )


def test_STA_indir_long_Y_in_A16_emits_word_store():
    # REP #$30 ; LDA #$abcd ; STA [$8d],Y ; RTS
    rom = bytes([
        0xC2, 0x30,         # REP #$30
        0xA9, 0xcd, 0xab,   # LDA #$abcd
        0x97, 0x8d,         # STA [$8d],Y
        0x60,               # RTS
    ])
    body = _emit(rom)
    assert 'IndirWriteByte' not in body, (
        'STA [dp],Y in M=0 must NOT emit IndirWriteByte. '
        f'Body:\n{body}'
    )
    assert 'IndirWriteWord' in body, (
        f'STA [dp],Y in M=0 must emit IndirWriteWord. Body:\n{body}'
    )


def test_STA_indir_long_in_M8_still_emits_byte_store():
    # Sanity: in M=1 (default on reset), STA [dp] is an 8-bit store.
    # LDA #$cd ; STA [$8d] ; RTS
    rom = bytes([
        0xA9, 0xcd,         # LDA #$cd  (M=1)
        0x87, 0x8d,         # STA [$8d]
        0x60,               # RTS
    ])
    body = _emit(rom)
    assert 'IndirWriteByte' in body, (
        f'STA [dp] in M=1 must emit IndirWriteByte. Body:\n{body}'
    )
    assert 'IndirWriteWord' not in body, (
        f'STA [dp] in M=1 must NOT emit IndirWriteWord. Body:\n{body}'
    )


def test_STA_indir_DP_in_A16_emits_word_store():
    # REP #$30 ; LDA #$abcd ; STA ($8d) ; RTS
    # DP_INDIR form — was silently dropped to a comment in the old emitter.
    rom = bytes([
        0xC2, 0x30,         # REP #$30
        0xA9, 0xcd, 0xab,   # LDA #$abcd
        0x92, 0x8d,         # STA ($8d)
        0x60,               # RTS
    ])
    body = _emit(rom)
    assert '*(uint16*)(IndirPtrDB' in body or 'IndirWriteWord' in body, (
        f'STA (dp) in M=0 must emit a 16-bit store. Body:\n{body}'
    )
    assert '/* STA16' not in body, (
        f'STA (dp) in M=0 must not fall through to an unhandled-mode comment. '
        f'Body:\n{body}'
    )


def test_STA_indir_DPY_in_A16_emits_word_store():
    # REP #$30 ; LDA #$abcd ; STA ($8d),Y ; RTS
    rom = bytes([
        0xC2, 0x30,         # REP #$30
        0xA9, 0xcd, 0xab,   # LDA #$abcd
        0x91, 0x8d,         # STA ($8d),Y
        0x60,               # RTS
    ])
    body = _emit(rom)
    assert '*(uint16*)(IndirPtrDB' in body or 'IndirWriteWord' in body, (
        f'STA (dp),Y in M=0 must emit a 16-bit store. Body:\n{body}'
    )
    assert '/* STA16' not in body


def test_STA_indir_DPX_in_A16_emits_word_store():
    # REP #$30 ; LDA #$abcd ; STA ($8d,X) ; RTS
    rom = bytes([
        0xC2, 0x30,         # REP #$30
        0xA9, 0xcd, 0xab,   # LDA #$abcd
        0x81, 0x8d,         # STA ($8d,X)
        0x60,               # RTS
    ])
    body = _emit(rom)
    assert '*(uint16*)(IndirPtrDB' in body or 'IndirWriteWord' in body, (
        f'STA (dp,X) in M=0 must emit a 16-bit store. Body:\n{body}'
    )
    assert '/* STA16' not in body
