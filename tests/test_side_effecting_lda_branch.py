"""LDA of a side-effecting hardware register + flag-branch must not re-emit the read.

Reproduces the SMW `I_IRQ` bug at ROM $00:8380-$8383:
  LDA.W HW_TIMEUP       ; $4211 — reading this register acks the IRQ
  BPL  ExitIRQ          ; branch on N flag

Previously emitted as:
    v1 = ReadReg(0x4211);
    if ((int8)(ReadReg(0x4211)) >= 0) goto label_...;

The second ReadReg($4211) re-read the MMIO port. $4211 (TIMEUP) clears
g_snes->inIrq on read, so the second read returned 0 and the BPL always
branched to exit — making the recompiled IRQ handler a no-op.

Root cause: `_branch_cond` builds the condition string from `self.flag_src`
before `_emit_branch` calls `_materialize('A', ...)`. Materialization updates
`self.A` to the newly-allocated var name but leaves `self.flag_src` holding
the original expression. Fix: `_materialize` now also updates `self.flag_src`
when it aliased the register value being materialized.

This test pins the fix against future regressions and also covers the
general "any ReadReg-sourced LDA followed by flag-consuming branch" pattern.
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


def test_LDA_TIMEUP_BPL_emits_single_ReadReg_call():
    # LDA.W $4211 ; BPL +2 ; LDA #$00 ; RTS
    # Opcodes: AD 11 42, 10 02, A9 00, 60
    rom = bytes([
        0xAD, 0x11, 0x42,  # LDA $4211 (HW_TIMEUP)
        0x10, 0x02,         # BPL +2 -> $8007
        0xA9, 0x00,         # LDA #$00
        0x60,               # RTS
    ])
    body = _emit(rom)
    n = body.count('ReadReg(0x4211)')
    assert n == 1, (
        f'LDA $4211 ; BPL must emit ReadReg(0x4211) exactly once; got {n} '
        f'(re-emission re-acks the IRQ, producing a handler that always '
        f'exits). Body:\n{body}'
    )


def test_LDA_TIMEUP_BMI_emits_single_ReadReg_call():
    # LDA.W $4211 ; BMI +2 ; LDA #$00 ; RTS
    rom = bytes([
        0xAD, 0x11, 0x42,  # LDA $4211
        0x30, 0x02,         # BMI +2
        0xA9, 0x00,
        0x60,
    ])
    body = _emit(rom)
    n = body.count('ReadReg(0x4211)')
    assert n == 1, (
        f'LDA $4211 ; BMI must emit ReadReg(0x4211) exactly once; got {n}\n{body}'
    )


def test_LDA_TIMEUP_BEQ_emits_single_ReadReg_call():
    # LDA.W $4211 ; BEQ +2 ; LDA #$00 ; RTS
    # BEQ uses the Z flag which is also derived from flag_src — same bug class.
    rom = bytes([
        0xAD, 0x11, 0x42,
        0xF0, 0x02,         # BEQ +2
        0xA9, 0x00,
        0x60,
    ])
    body = _emit(rom)
    n = body.count('ReadReg(0x4211)')
    assert n == 1, (
        f'LDA $4211 ; BEQ must emit ReadReg(0x4211) exactly once; got {n}\n{body}'
    )


def test_WRAM_LDA_BPL_still_works():
    # Sanity check: LDA of non-side-effecting source + branch still emits
    # valid code. Previously the emitter's behavior for WRAM reads was
    # correct (both reads return the same value); the fix must not break it.
    # LDA.B $10 ; BPL +2 ; LDA #$00 ; RTS
    rom = bytes([
        0xA5, 0x10,         # LDA $10 (direct page)
        0x10, 0x02,         # BPL +2
        0xA9, 0x00,
        0x60,
    ])
    body = _emit(rom)
    # Should contain a WRAM read somewhere and a valid BPL cond.
    assert 'g_ram' in body or '0x10' in body
    # Must not emit complaint/garbage.
    assert 'flags unknown' not in body
