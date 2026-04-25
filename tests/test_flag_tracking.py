"""Pinning tests for the flag-tracking fixes from the Phase B burndown
(2026-04-24). Each test reproduces a Phase B fuzz failure that the
fuzz harness caught and a corresponding emitter fix closed.

Covered:
  - CLV sets self.overflow='0' (was None) so BVC/BVS after CLV emit
    deterministic conditions.
  - SBC computes V via the signed-overflow formula (was None).
  - TSB/TRB compute Z from (A & pre_mem); preserve N from before;
    leave V untouched. Use n_src override so BPL/BMI read the
    preserved N source.
  - BIT separates N source (mem's top bit) from Z source (A & mem)
    via n_src.
  - _NO_FLAG_RESET extended to STA/STX/STY/STZ so a sequence
    `CPX #imm ; STZ $abs ; BPL ...` reads the X-width-correct
    flag_width on the BPL.
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


# ----- CLV --------------------------------------------------------------------

def test_CLV_then_BVC_emits_constant_branch():
    # CLV ; BVC +2 ; RTS
    # After CLV, V is known to be 0. BVC must emit `!(0)` (always taken),
    # not `/* !overflow? */ 0` (the old None-tracker output).
    rom = bytes([0xB8, 0x50, 0x02, 0x60, 0x60, 0x60])
    body = _emit_body(rom)
    assert '!(0)' in body or '!((0))' in body, (
        'BVC after CLV must emit !(0) — V is known clear; body:\n' + body)
    assert '/* !overflow?' not in body, (
        'BVC after CLV must NOT emit `/* !overflow? */` placeholder; body:\n' + body)


# ----- SBC V flag -------------------------------------------------------------

def test_SBC_emits_overflow_formula():
    # SEC ; SBC #$01 ; BVS +2 ; RTS
    # The BVS must reference a non-trivial overflow expression (not the
    # `/* overflow? */ 0` placeholder), so V is observable post-SBC.
    rom = bytes([
        0x38,                    # SEC
        0xE9, 0x01,              # SBC #$01
        0x70, 0x02,              # BVS
        0x60, 0x60, 0x60,
    ])
    body = _emit_body(rom)
    assert '/* overflow?' not in body, (
        'SBC must set self.overflow to a real expression; body:\n' + body)
    # The formula uses `& 0x80` (8-bit M=1) for the sign bit check.
    assert '& 0x80' in body, (
        'SBC V formula must include sign-bit mask; body:\n' + body)


def test_SBC_overflow_formula_is_16bit_at_M0():
    # REP #$20 ; SEC ; SBC #$0001 ; BVS +2 ; RTS
    rom = bytes([
        0xC2, 0x20,
        0x38,
        0xE9, 0x01, 0x00,        # SBC #$0001 (3-byte at M=0)
        0x70, 0x02,
        0x60, 0x60, 0x60,
    ])
    body = _emit_body(rom)
    assert '0x8000' in body, (
        '16-bit SBC V formula must use 0x8000 sign-bit mask; body:\n' + body)


# ----- TSB / TRB --------------------------------------------------------------

def test_TSB_DP_uses_pre_mem_for_Z_source():
    # LDA #$10 ; TSB $00 ; BEQ +2 ; RTS
    # Z must come from (A & pre_mem), not from the post-RMW value.
    # Look for a tmp = ... & ... pattern that isolates the Z source.
    rom = bytes([
        0xA9, 0x10,              # LDA #$10
        0x04, 0x00,              # TSB $00
        0xF0, 0x02,              # BEQ
        0x60, 0x60, 0x60,
    ])
    body = _emit_body(rom)
    # Look for "= (uint8)(<a>) & g_ram[..]" or similar Z-source compute.
    assert ' & g_ram[0x0]' in body or ' & RDB_LOAD8(0x0)' in body, (
        'TSB must compute Z from (A & pre_mem) using a separate tmp; body:\n'
        + body)


def test_TRB_DP_preserves_N_via_n_src():
    # LDA #$F0   ; sets N=1
    # LDX #$01   ; flag_src now from X
    # ...wait, simpler: just LDA #$F0 ; TRB $00 ; BMI +2 ; RTS
    # After TRB, N is whatever LDA #$F0 set (N=1). BMI condition must
    # reference the preserved N source (the LDA value 0xF0), not the
    # TRB result.
    rom = bytes([
        0xA9, 0xF0,              # LDA #$F0 (N=1, flag_src = 0xf0)
        0x14, 0x00,              # TRB $00 (N preserved)
        0x30, 0x02,              # BMI
        0x60, 0x60, 0x60,
    ])
    body = _emit_body(rom)
    # Look for a BMI cond that references 0xf0 (the LDA value, preserved
    # as N source through TRB).
    assert '(int8)0xf0 < 0' in body or '(int8)(0xf0) < 0' in body, (
        'TRB must preserve N from prior LDA via n_src; body:\n' + body)


# ----- BIT --------------------------------------------------------------------

def test_BIT_separates_N_source_from_Z_source():
    # LDA #$00 ; BIT $1234 ; BMI +2 ; BEQ +2 ; RTS
    # N comes from mem's top bit; Z from (A & mem). Different sources.
    rom = bytes([
        0xA9, 0x00,              # LDA #$00 (A=0)
        0x2C, 0x34, 0x12,        # BIT $1234 (ABS, M=1)
        0x30, 0x02,              # BMI (uses N)
        0xF0, 0x02,              # BEQ (uses Z)
        0x60, 0x60, 0x60,
    ])
    body = _emit_body(rom)
    # BMI condition must reference mem (g_ram[0x1234] or RDB_LOAD8(0x1234)),
    # NOT a (0 & mem) AND expression.
    bmi_idx = body.find('< 0')
    assert bmi_idx != -1, f'no BMI cond in body:\n{body}'
    # Look at the BMI line. It should sign-test mem alone.
    bmi_line_start = body.rfind('\n', 0, bmi_idx) + 1
    bmi_line = body[bmi_line_start:bmi_idx + 5]
    assert ' & ' not in bmi_line.replace('flags unknown', ''), (
        'BMI after BIT must sign-test mem alone (n_src), not (A & mem); '
        f'BMI line: {bmi_line!r}')


# ----- _NO_FLAG_RESET extension to STA/STX/STY/STZ ----------------------------

def test_CPX_STZ_BPL_preserves_X_flag_width():
    # SEP #$30 ; LDX #$F0 ; CPX #$42 ; STZ $1F00 ; BPL +2 ; RTS
    # After CPX (X-flag=1, flag_width=8), STZ must NOT reset
    # flag_width to 16. The BPL must emit (int8)... not (int16)...
    rom = bytes([
        0xE2, 0x30,              # SEP #$30
        0xA2, 0xF0,              # LDX #$F0
        0xE0, 0x42,              # CPX #$42
        0x9C, 0x00, 0x1F,        # STZ $1F00
        0x10, 0x02,              # BPL
        0x60, 0x60, 0x60,
    ])
    body = _emit_body(rom)
    # BPL after STZ must use (int8) cast — flag_width preserved from CPX.
    assert '(int8)' in body, (
        'BPL after CPX/STZ must use int8 sign cast (flag_width preserved); '
        'body:\n' + body)
    # And NOT (int16) for this BPL.
    bpl_idx = body.find('>= 0')
    assert bpl_idx != -1, f'no BPL cond in body:\n{body}'
    bpl_line_start = body.rfind('\n', 0, bpl_idx) + 1
    bpl_line = body[bpl_line_start:bpl_idx + 5]
    assert '(int16)' not in bpl_line, (
        'BPL must NOT use int16 cast at X-width=1; body line: ' + bpl_line)


def test_SEP_M0_to_M1_narrows_literal_A():
    # Phase B fuzz (2026-04-24) caught: REP #$20 ; LDA #$ABCD ;
    # SEP #$20 ; CMP #$CD ; BEQ +2 ; RTS — the BEQ should be taken
    # because A's low byte after SEP is $CD. Previously the emitter
    # only narrowed A when its hoisted type was uint16; literals
    # like '0xabcd' had no hoisted type, so the narrow was skipped
    # and the CMP evaluated `0xabcd - 0xcd = 0xab00 != 0`.
    rom = bytes([
        0xC2, 0x20,
        0xA9, 0xCD, 0xAB,        # LDA #$ABCD
        0xE2, 0x20,              # SEP #$20
        0xC9, 0xCD,              # CMP #$CD
        0xF0, 0x02,              # BEQ
        0x60, 0x60, 0x60,
    ])
    body = _emit_body(rom)
    assert '= (uint8)(0xabcd)' in body, (
        'SEP #$20 must narrow a literal-valued A from 16-bit to 8-bit '
        'so subsequent 8-bit ops see only the low byte; body:\n' + body)


def test_STA_does_not_reset_flag_width():
    # REP #$20 ; SEP #$10 ; CPX #$42 ; STA $00 ; BPL +2 ; RTS
    # M=0 X=1. CPX sets flag_width=8 (X-width). STA $00 must NOT
    # reset flag_width to 16 just because M=0.
    rom = bytes([
        0xC2, 0x20,
        0xE2, 0x10,
        0xA2, 0xF0,              # LDX #$F0 (X=8-bit)
        0xE0, 0x42,              # CPX #$42 (flag_width=8)
        0x85, 0x00,              # STA $00
        0x10, 0x02,              # BPL
        0x60, 0x60, 0x60,
    ])
    body = _emit_body(rom)
    bpl_idx = body.find('>= 0')
    assert bpl_idx != -1
    bpl_line_start = body.rfind('\n', 0, bpl_idx) + 1
    bpl_line = body[bpl_line_start:bpl_idx + 5]
    assert '(int8)' in bpl_line, (
        'BPL after CPX/STA must use int8 cast; body line: ' + bpl_line)
