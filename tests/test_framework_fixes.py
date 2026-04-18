"""Retroactive tests for recent framework fixes to the recompiler.

Pins the behavior introduced by these commits so future refactors surface
regressions instead of silently re-breaking the patterns:

  * BIT-for-V-flag idiom (27a4d0e): `BIT abs ; BVS/BVC` is V-flag-only,
    so BIT's incidental A-read is dead. Liveness must NOT promote A.
  * `preserves` cfg directive (407d617): parse_config recognizes
    `preserves <full_addr_hex> [A] [X] [Y]` and stores a register set.
  * Preserves hint overrides auto clobber (5b4e817):
    _augment_cfg_sigs_one_pass must prefer cfg.preserves[addr] over
    its heuristic `_writes_register_without_save_restore` result, so
    path-sensitive cases (tail-jumps that write a register on the
    non-returning path) can be communicated without a full CFG analysis.
"""
import os
import pathlib
import sys
import tempfile
import textwrap

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / 'recompiler'))

import recomp  # noqa: E402
from snes65816 import decode_insn  # noqa: E402


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


# ---------------------------------------------------------------------------
# BIT-for-V-flag idiom
# ---------------------------------------------------------------------------

def test_BIT_abs_followed_by_BVC_is_dead_A_read():
    # BIT $1234 ; BVC +2 ; RTS
    # Opcode table: BIT abs = 0x2C, BVC = 0x50.
    rom = bytes([0x2C, 0x34, 0x12, 0x50, 0x00, 0x60])
    insns = _build_insns(rom, 0x8000)
    li = recomp.infer_live_in_regs(insns, 0x8000)
    assert li['A'] is None, (
        f'BIT;BVC reads A only for the (dead) Z flag; A must not be '
        f'live-in, got A={li["A"]}'
    )


def test_BIT_abs_followed_by_BVS_is_dead_A_read():
    # BIT $1234 ; BVS +2 ; RTS
    rom = bytes([0x2C, 0x34, 0x12, 0x70, 0x00, 0x60])
    insns = _build_insns(rom, 0x8000)
    li = recomp.infer_live_in_regs(insns, 0x8000)
    assert li['A'] is None, (
        f'BIT;BVS pattern must not mark A as live-in, got A={li["A"]}'
    )


def test_BIT_abs_followed_by_BEQ_still_reads_A():
    # BIT $1234 ; BEQ +2 ; RTS
    # BEQ depends on Z = (A & mem == 0), which DOES depend on A, so
    # the BIT read IS live and A must show as live-in.
    rom = bytes([0x2C, 0x34, 0x12, 0xF0, 0x00, 0x60])
    insns = _build_insns(rom, 0x8000)
    li = recomp.infer_live_in_regs(insns, 0x8000)
    assert li['A'] == 8, (
        f'BIT;BEQ tests Z which depends on A; A must be live-in, '
        f'got A={li["A"]}'
    )


def test_BIT_abs_followed_by_BNE_still_reads_A():
    # BIT $1234 ; BNE +2 ; RTS
    rom = bytes([0x2C, 0x34, 0x12, 0xD0, 0x00, 0x60])
    insns = _build_insns(rom, 0x8000)
    li = recomp.infer_live_in_regs(insns, 0x8000)
    assert li['A'] == 8, (
        f'BIT;BNE also Z-dependent, A must be live-in, got A={li["A"]}'
    )


# ---------------------------------------------------------------------------
# `preserves` cfg directive
# ---------------------------------------------------------------------------

def _write_tmp_cfg(content: str) -> str:
    fd, path = tempfile.mkstemp(suffix='.cfg', prefix='test_preserves_')
    os.close(fd)
    with open(path, 'w') as f:
        f.write(content)
    return path


def test_preserves_directive_parses_all_three_registers():
    path = _write_tmp_cfg(textwrap.dedent('''\
        bank = 00
        preserves 7F8000 A X Y
    '''))
    try:
        cfg = recomp.parse_config(path)
        assert 0x7F8000 in cfg.preserves
        assert cfg.preserves[0x7F8000] == {'A', 'X', 'Y'}
    finally:
        os.unlink(path)


def test_preserves_directive_parses_subset():
    path = _write_tmp_cfg(textwrap.dedent('''\
        bank = 00
        preserves 00EE1D X Y
    '''))
    try:
        cfg = recomp.parse_config(path)
        assert cfg.preserves[0x00EE1D] == {'X', 'Y'}
    finally:
        os.unlink(path)


def test_preserves_directive_case_insensitive():
    # `a`, `x`, `y` lower-case should be accepted and normalised.
    path = _write_tmp_cfg(textwrap.dedent('''\
        bank = 00
        preserves 1234 a y
    '''))
    try:
        cfg = recomp.parse_config(path)
        assert cfg.preserves[0x1234] == {'A', 'Y'}
    finally:
        os.unlink(path)


def test_preserves_directive_empty_list_is_preserves_nothing():
    # `preserves 1234` with no registers = callee preserves nothing
    # (equivalent to default conservative clobber-all).
    path = _write_tmp_cfg(textwrap.dedent('''\
        bank = 00
        preserves 1234
    '''))
    try:
        cfg = recomp.parse_config(path)
        assert cfg.preserves[0x1234] == set()
    finally:
        os.unlink(path)


# ---------------------------------------------------------------------------
# Preserves hint overrides auto-clobber heuristic
# ---------------------------------------------------------------------------

def test_writes_register_heuristic_flags_body_that_writes_Y():
    # Sanity: the baseline heuristic says `LDY #$20 ; RTS` clobbers Y,
    # because there's no PHY/PLY save-restore bracket. This test pins
    # the baseline behavior that preserves-override is supposed to
    # countermand.
    rom = bytes([0xA0, 0x20, 0x60])  # LDY #$20 ; RTS
    insns = _build_insns(rom, 0x8000)
    assert recomp._writes_register_without_save_restore(insns, 'Y')


def test_augment_respects_preserves_override():
    # When cfg.preserves has an entry for a function, it wins over the
    # write-without-save-restore heuristic. This is what lets
    # RunPlayerBlockCode_00EE1D communicate that its LDY-on-a-tail-
    # jump-path is not a caller-visible Y clobber.
    rom_bytes = bytearray(0x20000)
    # LoROM: bank 0 PC $8000 maps to ROM offset 0 (bank << 15 | pc & 0x7FFF).
    # Body: LDY #$20 ; RTS.
    rom_bytes[0] = 0xA0; rom_bytes[1] = 0x20; rom_bytes[2] = 0x60
    rom = bytes(rom_bytes)
    cfg = recomp.Config()
    cfg.bank = 0
    cfg.funcs.append(('some_func', 0x8000, 'void()', 0x8003, {}, {}))
    cfg.preserves[0x008000] = {'Y'}

    recomp._augment_cfg_sigs_one_pass(rom, cfg)

    # Y must not be in the clobber set because cfg.preserves said so,
    # overriding the heuristic that would have flagged Y on the raw
    # body.
    assert 'Y' not in cfg.clobbers[0x008000], (
        f'preserves hint should override auto-clobber, got clobbers='
        f'{cfg.clobbers[0x008000]}'
    )


def test_augment_without_preserves_uses_heuristic():
    # Same body (LDY #$20 ; RTS) without a preserves hint: Y should
    # appear in the auto-computed clobber set.
    rom_bytes = bytearray(0x20000)
    # LoROM: bank 0 PC $8000 maps to ROM offset 0.
    rom_bytes[0] = 0xA0; rom_bytes[1] = 0x20; rom_bytes[2] = 0x60
    rom = bytes(rom_bytes)
    cfg = recomp.Config()
    cfg.bank = 0
    cfg.funcs.append(('some_func', 0x8000, 'void()', 0x8003, {}, {}))

    recomp._augment_cfg_sigs_one_pass(rom, cfg)

    assert 'Y' in cfg.clobbers[0x008000], (
        f'no preserves hint + body writes Y => heuristic should flag Y, '
        f'got clobbers={cfg.clobbers[0x008000]}'
    )
