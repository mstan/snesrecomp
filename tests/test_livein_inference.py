"""Structural tests for the recompiler's ROM-derived live-in / clobber
inference. Each test loads a small function from the SMW ROM, runs the
inference pass, and asserts on the result.

Covered invariants:
  - A function that reads A before writing it is live-in on A.
  - A function that reads X before writing it (via indexed addressing)
    is live-in on X.
  - A trampoline `JSR $foo ; RTS` inherits its callee's live-in set via
    the JSR-consumes-callee-params rule.
  - PHA / PHX / PHY alone do NOT count as live-in reads (they're
    save-restore, not consumption).
  - A function that writes Y with no PHY/PLY bracket clobbers Y.
  - A function with matching PHY...PLY does NOT clobber Y.
"""
import pathlib
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / 'recompiler'))

import recomp  # noqa: E402
from snes65816 import decode_insn, Insn  # noqa: E402


def _build_insns(rom_bytes: bytes, start_pc: int, bank: int = 0,
                  m: int = 1, x: int = 1) -> list:
    """Decode a short synthetic ROM snippet into Insn list."""
    out = []
    off = 0
    pc = start_pc
    while off < len(rom_bytes):
        insn = decode_insn(rom_bytes, off, pc, bank, m=m, x=x)
        if insn is None:
            break
        # Update m/x trackers for REP/SEP so 16-bit A reads show correct flags.
        if insn.mnem == 'REP':
            if insn.operand & 0x20: m = 0
            if insn.operand & 0x10: x = 0
        elif insn.mnem == 'SEP':
            if insn.operand & 0x20: m = 1
            if insn.operand & 0x10: x = 1
        insn.m_flag = m
        insn.x_flag = x
        out.append(insn)
        off += insn.length
        pc = (pc + insn.length) & 0xFFFF
        if insn.mnem in ('RTS', 'RTL', 'RTI'):
            break
    return out


def test_live_in_A_when_read_before_write():
    # STA $12 ; RTS   ← reads A at entry.
    rom = bytes([0x85, 0x12, 0x60])
    insns = _build_insns(rom, 0x8000)
    li = recomp.infer_live_in_regs(insns, 0x8000)
    assert li['A'] == 8, f'A should be 8-bit live-in, got {li["A"]}'
    assert li['X'] is None, f'X should not be live-in, got {li["X"]}'
    assert li['Y'] is None, f'Y should not be live-in, got {li["Y"]}'


def test_no_live_in_when_LDA_precedes_STA():
    # LDA #$7F ; STA $12 ; RTS   ← A is written before any read.
    rom = bytes([0xA9, 0x7F, 0x85, 0x12, 0x60])
    insns = _build_insns(rom, 0x8000)
    li = recomp.infer_live_in_regs(insns, 0x8000)
    assert li['A'] is None, f'A should NOT be live-in after LDA, got {li["A"]}'


def test_PHX_alone_is_not_live_in_read():
    # PHX ; LDX #$00 ; PLX ; RTS
    # Common save-restore pattern: PHX/PLX bracket an LDX. The PHX
    # should NOT count as a read that makes X live-in, otherwise every
    # preserve-X helper falsely gains a uint8_k param.
    rom = bytes([0xDA, 0xA2, 0x00, 0xFA, 0x60])
    insns = _build_insns(rom, 0x8000)
    li = recomp.infer_live_in_regs(insns, 0x8000)
    assert li['X'] is None, (
        f'PHX should be ignored for liveness (save-restore), got X={li["X"]}'
    )


def test_indexed_mode_counts_as_X_read():
    # LDA $1000,X ; RTS   ← indexed reads X.
    rom = bytes([0xBD, 0x00, 0x10, 0x60])
    insns = _build_insns(rom, 0x8000)
    li = recomp.infer_live_in_regs(insns, 0x8000)
    assert li['X'] == 8, f'X should be live-in via abs,X, got {li["X"]}'


def test_trampoline_inherits_callee_live_in():
    # JSR $8100 ; RTS with callee declared to take uint8_k.
    rom = bytes([0x20, 0x00, 0x81, 0x60])
    insns = _build_insns(rom, 0x8000, bank=0)
    callee_sigs = {0x008100: 'void(uint8_k)'}
    li = recomp.infer_live_in_regs(insns, 0x8000, bank=0,
                                    callee_sigs=callee_sigs)
    assert li['X'] == 8, (
        'Trampoline JSR $8100 should inherit X live-in from callee '
        f'sig void(uint8_k), got X={li["X"]}'
    )


def test_modifies_Y_without_restore():
    # LDY #$00 ; INY ; RTS   ← writes Y, no PHY/PLY.
    rom = bytes([0xA0, 0x00, 0xC8, 0x60])
    insns = _build_insns(rom, 0x8000)
    assert recomp._writes_register_without_save_restore(insns, 'Y'), (
        'LDY/INY without PHY/PLY should register as a Y clobber'
    )


def test_PHY_PLY_means_Y_preserved():
    # PHY ; INY ; PLY ; RTS   ← Y modified inside but save-restored.
    rom = bytes([0x5A, 0xC8, 0x7A, 0x60])
    insns = _build_insns(rom, 0x8000)
    assert not recomp._writes_register_without_save_restore(insns, 'Y'), (
        'PHY/PLY should bracket writes — Y is preserved from callerview'
    )


def test_memory_save_restore_counts_as_preserve():
    # STY $03 ; LDA #$55 ; TAY ; LDY $03 ; RTS
    # Classic STR/LDR save-restore idiom ($00:86DF pattern). Even though
    # TAY writes Y in the middle, the final LDY $03 restores caller's Y.
    rom = bytes([
        0x84, 0x03,  # STY $03
        0xA9, 0x55,  # LDA #$55
        0xA8,        # TAY (writes Y)
        0xA4, 0x03,  # LDY $03
        0x60,        # RTS
    ])
    insns = _build_insns(rom, 0x8000)
    assert not recomp._writes_register_without_save_restore(insns, 'Y'), (
        'STY $03 ... LDY $03 save-restore should preserve Y'
    )


def test_ldx_before_rts_restore_expr():
    # LDA $12 ; TAX ; LDX $1698 ; RTS
    # $1698 is the WRAM slot holding caller's sprite index. Final LDX
    # restores X from there, so caller's X is preserved through the call.
    rom = bytes([
        0xA5, 0x12,              # LDA $12
        0xAA,                    # TAX (writes X)
        0xAE, 0x98, 0x16,        # LDX $1698
        0x60,                    # RTS
    ])
    insns = _build_insns(rom, 0x8000)
    expr = recomp._detect_x_restore_expr(insns)
    assert expr == 'g_ram[0x1698]', (
        f'LDX $1698 ; RTS should detect g_ram[0x1698] restore, got {expr!r}'
    )


def test_ldx_restore_walks_past_index_uses():
    # LDX $1698 ; STA $16A1,X ; STA $16A9,X ; RTS
    # Intervening X-indexed stores read X but don't modify it. The final
    # LDX result survives to the RTS.
    rom = bytes([
        0xAE, 0x98, 0x16,        # LDX $1698
        0x9D, 0xA1, 0x16,        # STA $16A1,X (reads X)
        0x9D, 0xA9, 0x16,        # STA $16A9,X (reads X)
        0x60,                    # RTS
    ])
    insns = _build_insns(rom, 0x8000)
    expr = recomp._detect_x_restore_expr(insns)
    assert expr == 'g_ram[0x1698]', (
        f'LDX followed only by X-indexed reads should still register as a'
        f' restore; got {expr!r}'
    )


def test_carry_return_detects_clc_rts():
    # CLC ; RTS — the "no contact" bool-via-carry idiom.
    rom = bytes([0x18, 0x60])  # CLC, RTS
    insns = _build_insns(rom, 0x8000)
    assert recomp._looks_like_carry_return(insns), (
        'CLC ; RTS should be recognised as a carry-return helper'
    )


def test_carry_return_detects_sec_rts():
    rom = bytes([0x38, 0x60])  # SEC, RTS
    insns = _build_insns(rom, 0x8000)
    assert recomp._looks_like_carry_return(insns), (
        'SEC ; RTS should be recognised as a carry-return helper'
    )


def test_carry_return_rejects_lda_rts():
    # LDA #$55 ; RTS — returns A explicitly, not carry.
    rom = bytes([0xA9, 0x55, 0x60])
    insns = _build_insns(rom, 0x8000)
    assert not recomp._looks_like_carry_return(insns), (
        'LDA ; RTS writes A and should NOT be a carry-return helper'
    )


def test_ldx_restore_bails_on_non_ldx_writer():
    # LDX $1698 ; TAX ; RTS
    # Here TAX writes X after the LDX, so X at RTS is NOT g_ram[0x1698].
    # Restore detector must bail.
    rom = bytes([
        0xAE, 0x98, 0x16,        # LDX $1698
        0xAA,                    # TAX (overwrites X)
        0x60,                    # RTS
    ])
    insns = _build_insns(rom, 0x8000)
    expr = recomp._detect_x_restore_expr(insns)
    assert expr is None, (
        f'TAX between LDX and RTS clobbers the restore; detector should'
        f' bail. Got {expr!r}'
    )
