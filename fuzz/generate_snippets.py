"""Generate snippet ROM fragments for Phase B differential fuzzing.

For every opcode × addressing mode × (M, X) pairing in the opcode
table, emit a set of snippets with seeded input states.

A snippet is a dict:
  {
    "id": "LDA_IMM_M1_X1_seed0",
    "opcode": 0xA9,
    "mnem": "LDA",
    "mode": "IMM",
    "m_flag": 1,
    "x_flag": 1,
    "rom": [byte, byte, ...],      # prologue + test insn + RTS
    "initial_cpu": {               # seeded CPU state before running
      "A": 0x1234, "X": 0x00, "Y": 0x00,
      "D": 0x0000, "DB": 0x7E, "S": 0x01FF,
      "P": 0x30,                   # M=1 X=1 by default
    },
    "initial_wram": {              # optional: {addr: byte, ...}
      "0x0000": 0xAA, "0x0001": 0xBB,
    },
    "expected_touched_regs": ["A", "flags"],
    "expected_touched_wram": [0x0000, 0x0001],
  }

The runner takes this snippet, runs it through both recomp and
snes9x, and diffs final state.

Scope of this step: generate snippets for the opcodes whose
codegen we've edited or plan to edit. Start narrow (ASL/LSR/ROL/
ROR + LDA/STA + TSB/TRB + BIT), expand later. The goal is a
harness that WORKS end-to-end on a small slice, then scales.
"""
from __future__ import annotations
import json
import pathlib
import sys

FUZZ_DIR = pathlib.Path(__file__).resolve().parent
OPCODE_TABLE = FUZZ_DIR / 'opcode_table.json'
OUTPUT_DIR = FUZZ_DIR / 'snippets'


# ----- Seed inputs per mnemonic ----------------------------------------------
# Each seed is a dict of initial register + WRAM state. Different seeds
# exercise different carry/sign/zero edges.

def seeds_for(mnem: str, mode: str, m_flag: int, x_flag: int):
    """Return a list of (name, initial_state) tuples for this opcode.
    initial_state is {'A': ..., 'X': ..., 'Y': ..., 'carry': 0/1,
                      'wram': {addr: byte}}.
    """
    a_mask = 0xFFFF if m_flag == 0 else 0xFF
    x_mask = 0xFFFF if x_flag == 0 else 0xFF

    # Arithmetic/shift/rotate: seed accumulator + carry-in.
    if mnem in ('ADC', 'SBC'):
        return [
            ('zero',      {'A': 0, 'carry': 0}),
            ('plus_one',  {'A': 1, 'carry': 0}),
            ('signbit',   {'A': a_mask ^ (a_mask >> 1), 'carry': 0}),
            ('max',       {'A': a_mask, 'carry': 0}),
            ('max_c1',    {'A': a_mask, 'carry': 1}),
        ]
    if mnem in ('ASL', 'LSR'):
        return [
            ('zero',       {'A': 0, 'carry': 0}),
            ('lobit',      {'A': 1, 'carry': 0}),
            ('hibit',      {'A': a_mask ^ (a_mask >> 1), 'carry': 0}),
            ('all_ones',   {'A': a_mask, 'carry': 0}),
        ]
    if mnem in ('ROL', 'ROR'):
        return [
            ('zero_c0',     {'A': 0, 'carry': 0}),
            ('zero_c1',     {'A': 0, 'carry': 1}),
            ('hibit_c0',    {'A': a_mask ^ (a_mask >> 1), 'carry': 0}),
            ('hibit_c1',    {'A': a_mask ^ (a_mask >> 1), 'carry': 1}),
            ('lobit_c1',    {'A': 1, 'carry': 1}),
        ]
    if mnem in ('AND', 'ORA', 'EOR'):
        return [
            ('zero_zero',   {'A': 0}),
            ('all_all',     {'A': a_mask}),
            ('ff_55',       {'A': 0xFF & a_mask}),
        ]
    if mnem in ('CMP', 'CPX', 'CPY'):
        return [
            ('eq',          {'A': 0x42, 'X': 0x42, 'Y': 0x42}),
            ('gt',          {'A': 0xF0, 'X': 0xF0, 'Y': 0xF0}),
            ('lt',          {'A': 0x01, 'X': 0x01, 'Y': 0x01}),
        ]
    if mnem in ('LDA', 'LDX', 'LDY', 'STA', 'STX', 'STY', 'STZ'):
        return [
            ('seed_a',      {'A': 0x1234 & a_mask, 'X': 0x56 & x_mask, 'Y': 0x78 & x_mask}),
        ]
    if mnem in ('INC', 'DEC', 'INX', 'INY', 'DEX', 'DEY'):
        # For indexed-memory INC/DEC, large X drives the effective
        # address into bank-$00 ROM space where snes9x NOPs writes
        # and recomp writes WRAM (different class — needs bank-aware
        # dispatch). Keep X small for memory-indexed cases; full X
        # range only for register INC/DEC.
        is_register_only = (mnem in ('INX', 'INY', 'DEX', 'DEY')
                            or mode == 'ACC')
        if is_register_only:
            return [
                ('zero',        {'A': 0, 'X': 0, 'Y': 0}),
                ('boundary',    {'A': a_mask, 'X': x_mask, 'Y': x_mask}),
                ('signwrap',    {'A': a_mask ^ (a_mask >> 1), 'X': x_mask ^ (x_mask >> 1)}),
            ]
        # Memory indexed: keep X/Y small enough that DP+X stays in WRAM.
        return [
            ('zero',        {'A': 0, 'X': 0, 'Y': 0}),
            ('small_idx',   {'A': a_mask, 'X': 0x10, 'Y': 0x10}),
            ('near_wrap',   {'A': a_mask ^ (a_mask >> 1), 'X': 0xF0, 'Y': 0xF0}),
        ]
    if mnem in ('BIT',):
        return [
            ('bits_clear',  {'A': 0}),
            ('bits_set',    {'A': a_mask}),
        ]
    if mnem in ('TSB', 'TRB'):
        return [
            ('A_zero',      {'A': 0}),
            ('A_all',       {'A': a_mask}),
        ]
    if mnem in ('TAX', 'TAY', 'TXA', 'TYA', 'TXY', 'TYX'):
        return [
            ('seed_xfer',   {'A': 0x1234 & a_mask, 'X': 0x56 & x_mask, 'Y': 0x78 & x_mask}),
        ]
    if mnem == 'XBA':
        return [
            ('hilo',        {'A': 0x1234 & a_mask}),
        ]
    # Default: a single seed with mid-range values.
    return [
        ('default',       {'A': 0x0042, 'X': 0x05, 'Y': 0x06}),
    ]


# ----- Instruction encoders --------------------------------------------------
# Return the raw bytes for an instruction at a given addressing mode.
# Operand values are fixed per-mode so the snippet knows which WRAM address
# is touched.

# Chosen operand addresses. DP mode uses $10 (avoiding scratch $00-$0F which
# the decoder and emitter use as parameter-passing area). ABS uses $0100.
# Indirect modes need a SEPARATE DP slot ($20) holding a pointer to WRAM,
# because the regular $10 baseline (0xAA55) would point at ROM. The
# fuzz harness pre-seeds $20/$21 = $00/$01 (pointer to $0100) and
# $22 = $00 (high byte for INDIR_L/INDIR_LY → bank $00 WRAM mirror).
DP_OPERAND = 0x10
DP_INDIR_OPERAND = 0x20  # holds 16-bit pointer (or 24-bit including $22)
ABS_OPERAND = 0x0100
LONG_OPERAND = 0x7E0200  # bank $7E WRAM
REL_OPERAND = 0x00        # forward branch = 0 (skip nothing)
IMM_OPERAND_8 = 0x42
IMM_OPERAND_16 = 0x1234


def encode_insn(opcode: int, mode: str, m_flag: int, x_flag: int, mnem: str) -> bytes:
    """Encode the test instruction bytes for the given opcode + mode."""
    if mode == 'IMP' or mode == 'ACC':
        return bytes([opcode])
    if mode == 'IMM':
        # Width depends on M for most; X for LDX/LDY/CPX/CPY.
        if mnem in ('LDX', 'LDY', 'CPX', 'CPY'):
            wide = (x_flag == 0)
        elif mnem in ('REP', 'SEP'):
            wide = False  # always 1-byte imm
        else:
            wide = (m_flag == 0)
        if wide:
            return bytes([opcode, IMM_OPERAND_16 & 0xFF, (IMM_OPERAND_16 >> 8) & 0xFF])
        return bytes([opcode, IMM_OPERAND_8])
    if mode in ('DP_INDIR', 'INDIR_Y', 'INDIR_L', 'INDIR_LY', 'INDIR_DPX'):
        # Indirect modes read pointer from DP_INDIR_OPERAND; that slot
        # is pre-seeded by the harness to point at WRAM $0100.
        return bytes([opcode, DP_INDIR_OPERAND])
    if mode in ('DP', 'DP_X', 'DP_Y', 'STK', 'STK_IY'):
        return bytes([opcode, DP_OPERAND])
    if mode in ('ABS', 'ABS_X', 'ABS_Y', 'INDIR', 'INDIR_X'):
        return bytes([opcode, ABS_OPERAND & 0xFF, (ABS_OPERAND >> 8) & 0xFF])
    if mode in ('LONG', 'LONG_X'):
        return bytes([opcode, LONG_OPERAND & 0xFF, (LONG_OPERAND >> 8) & 0xFF,
                      (LONG_OPERAND >> 16) & 0xFF])
    if mode == 'REL':
        return bytes([opcode, REL_OPERAND])
    if mode == 'REL16':
        return bytes([opcode, 0x00, 0x00])
    raise ValueError(f'unknown mode {mode}')


def prologue(m_flag: int, x_flag: int, seed: dict) -> bytes:
    """Emit REP/SEP + LDA/LDX/LDY to set M/X flags and seed registers.

    Always emits REP #$30 first to normalize (both 16-bit), then SEP
    the bits that should be 1 in the target state.

    ALWAYS seeds A/X/Y (defaulting to 0 if the seed doesn't name them)
    so the recomp emitter has a tracked value on entry. Without this,
    INX/DEX/etc on an unseeded register become no-ops in the emitter
    but decrement at hardware time, producing spurious divergences.
    """
    out = bytearray()
    # REP #$30 — clear both M and X (go to 16-bit)
    out += bytes([0xC2, 0x30])
    # Now SEP the bits we want as 1.
    sep_mask = (m_flag & 1) << 5 | (x_flag & 1) << 4
    if sep_mask:
        out += bytes([0xE2, sep_mask])
    # Seed A with LDA #imm (default 0).
    val = seed.get('A', 0)
    if m_flag == 0:
        out += bytes([0xA9, val & 0xFF, (val >> 8) & 0xFF])
    else:
        out += bytes([0xA9, val & 0xFF])
    # Seed X (default 0).
    val = seed.get('X', 0)
    if x_flag == 0:
        out += bytes([0xA2, val & 0xFF, (val >> 8) & 0xFF])
    else:
        out += bytes([0xA2, val & 0xFF])
    # Seed Y (default 0).
    val = seed.get('Y', 0)
    if x_flag == 0:
        out += bytes([0xA0, val & 0xFF, (val >> 8) & 0xFF])
    else:
        out += bytes([0xA0, val & 0xFF])
    # Seed carry (default: CLC, so C always starts at 0 rather than
    # inheriting whatever snes9x had from the previous frame).
    # Normalizing makes the flag-capture epilogue produce deterministic
    # results across snippets that don't themselves modify C.
    if seed.get('carry') == 1:
        out += bytes([0x38])  # SEC
    else:
        out += bytes([0x18])  # CLC
    # Always CLV so V starts at 0, for the same reason.
    out += bytes([0xB8])      # CLV
    return bytes(out)


# ----- Scope filter -----------------------------------------------------------
# Start narrow: mnemonics whose codegen we've audited or edited. Expand later.

SCOPE_MNEMS = {
    # Phase A targets.
    'ASL', 'LSR', 'ROL', 'ROR',
    'TSB', 'TRB',
    'BIT',
    # Core arithmetic/logic — most likely to shake out more width bugs.
    'ADC', 'SBC',
    'AND', 'ORA', 'EOR',
    'CMP', 'CPX', 'CPY',
    # Load/store — baseline coverage.
    'LDA', 'LDX', 'LDY',
    'STA', 'STX', 'STY', 'STZ',
    'INC', 'DEC', 'INX', 'DEX', 'INY', 'DEY',
    # Transfers.
    'TAX', 'TAY', 'TXA', 'TYA', 'TXY', 'TYX',
    # Phase B #5 additions — single-instruction mnemonics whose codegen
    # is real (not a no-op stub) and whose effects are observable in
    # A/X/Y/flags.
    'XBA',
    'CLC', 'SEC', 'CLD', 'SED', 'CLI', 'SEI', 'CLV',
}

# Modes covered by the fuzz. Phase B #4 (2026-04-24) added the
# indirect modes (DP_INDIR / INDIR_Y / INDIR_DPX) and DP_Y / ABS_Y /
# LONG / LONG_X. Each new mode bucket exercises a distinct codegen
# path through _resolve_mem and per-mode branches in STA/STX/STY/LDA.
#
# INDIR_L / INDIR_LY (24-bit-indirect-via-long-pointer) are out of
# scope for v0.2 — they require modeling the runtime's LongPtr
# struct in the fuzz harness, which is invasive. Re-add when the
# fuzz harness gets a proper LongPtr abstraction.
SCOPE_MODES = {
    'IMP', 'ACC', 'IMM',
    'DP', 'DP_X', 'DP_Y',
    'ABS', 'ABS_X', 'ABS_Y',
    'INDIR_Y', 'INDIR_DPX', 'DP_INDIR',
    'LONG', 'LONG_X',
}


def in_scope(e: dict) -> bool:
    return e['mnem'] in SCOPE_MNEMS and e['mode'] in SCOPE_MODES


# ----- Main -------------------------------------------------------------------

def epilogue(m_flag_after: int, x_flag_after: int) -> bytes:
    """After the test instruction, snapshot A/X/Y AND capture the
    four observable CPU flags (N, V, Z, C) into reserved WRAM so
    both recomp and oracle produce a comparable output even for
    register-only and flag-only opcodes.

    Epilogue addresses (baseline: all 0xFF from the fuzz harness):
      $1F00-$1F01  final A (word or byte, written by STA)
      $1F02-$1F03  final X (word or byte, STX)
      $1F04-$1F05  final Y (word or byte, STY)
      $1F06        Carry:    0 if C set, 0xFF if clear
      $1F07        Zero:     0 if Z set, 0xFF if clear
      $1F08        Negative: 0 if N set, 0xFF if clear
      $1F09        Overflow: 0 if V set, 0xFF if clear

    Flag-capture technique: branch-conditional STZ. STZ $abs does
    not modify flags, and conditional branches don't modify flags
    either, so all four flag captures read the post-test-insn flag
    state directly. Pattern:

        BCC +3      ; if carry CLEAR, skip the STZ (leave slot 0xFF)
        STZ $1F06   ; if we reach here, carry was SET — write 0

    This avoids the PHP/PLA path, which doesn't work in the recomp
    emitter: recomp doesn't synthesize a P byte (flag_src is a
    value, not a packed P register), so PLA after PHP returns the
    flag_src EXPRESSION instead of a real P byte.

    Order: flag captures FIRST (while test insn's flags are fresh),
    then A/X/Y snapshots last. (STA/STX/STY don't modify flags, but
    the prologue's register-seed LDA/LDX/LDY don't either — we
    just want to minimize distance between the test insn and the
    flag captures.)
    """
    out = bytearray()
    # Register snapshots FIRST (at current test-insn M/X widths), so the
    # A/X/Y values captured reflect the test insn's output width. Must
    # come before the SEP #$20 below — SEP wouldn't change A/X/Y but
    # narrows how STA serializes.
    out += bytes([0x8D, 0x00, 0x1F])    # STA $1F00
    out += bytes([0x8E, 0x02, 0x1F])    # STX $1F02
    out += bytes([0x8C, 0x04, 0x1F])    # STY $1F04
    # Force M=1 (8-bit A) for the flag captures. In M=0, STZ ABS
    # writes a WORD, so STZ $1F06 clobbers both $1F06 AND $1F07 —
    # the carry slot's STZ would wipe the zero slot too. SEP #$20
    # preserves N/V/Z/C (it only sets the M flag), so the captures
    # still read the test insn's fresh flag state.
    out += bytes([0xE2, 0x20])          # SEP #$20
    # BCC +3 skips STZ if carry clear → slot stays 0xFF (baseline).
    #   branch_op is the "branch if flag CLEAR" variant.
    flag_captures = [
        (0x90, 0x1F06),  # BCC; C flag
        (0xD0, 0x1F07),  # BNE; Z flag
        (0x10, 0x1F08),  # BPL; N flag
        (0x50, 0x1F09),  # BVC; V flag
    ]
    for branch_op, addr in flag_captures:
        out += bytes([branch_op, 0x03])                       # B__ +3 skip STZ
        out += bytes([0x9C, addr & 0xFF, (addr >> 8) & 0xFF]) # STZ abs
    return bytes(out)


def compound_snippets():
    """Generate multi-instruction sequences that test interactions
    between mnemonics — particularly width transitions and stack
    push/pop pairs.

    Phase B #6: width-mismatched push/pop. The recomp emitter tracks
    stack entries as (reg, value) tuples, not bytes, so a PHA at M=0
    pushed as one element is popped by PLA at M=1 as one element —
    but on hardware the byte counts differ. The audit #7 detection
    code emits a RECOMP_WARN comment; this fuzz seed confirms whether
    the runtime actually diverges.
    """
    out = []

    # Phase B #6 width-mismatched PHA/PLA / PHX/PLX snippets removed
    # 2026-04-24: a byte-aware stack refactor regressed SMW visually
    # at the gameplay level. The audit #7 detection-only path (RECOMP_WARN
    # comments at every mismatched pull) remains in place. Re-add these
    # compound tests when the byte-aware stack is re-implemented in a
    # way that preserves the matched-width path's existing tracker
    # semantics (load-bearing for SMW's PHX/PLX register-save idiom
    # which the byte-aware push lost the orig_var bookkeeping for).

    # XBA round-trip — fully 8-bit so all immediates are 1-byte.
    rom = bytearray()
    rom += bytes([0xE2, 0x30])           # SEP #$30 — A and X to 8-bit
    rom += bytes([0xA9, 0x34])           # LDA #$34 (A=$34)
    rom += bytes([0xEB])                 # XBA — A=$00, B=$34
    rom += bytes([0xA9, 0x12])           # LDA #$12
    rom += bytes([0xEB])                 # XBA — A=$34, B=$12
    rom += bytes([0xA2, 0x00])           # LDX #0 (1-byte at X=8-bit)
    rom += bytes([0xA0, 0x00])           # LDY #0
    rom += bytes([0x18])
    rom += bytes([0xB8])
    out.append({
        'id': 'compound_XBA_round_trip',
        'opcode': 0xEB, 'mnem': 'XBA', 'mode': 'IMP',
        'm_flag': 1, 'x_flag': 1,
        'seed_name': 'round_trip',
        'seed': {'A': 0, 'X': 0, 'Y': 0},
        'rom_hex': bytes(rom).hex(),
        'touches_A': True, 'touches_X': False, 'touches_Y': False,
        'reads_mem': False, 'writes_mem': False,
        '_compound': True,
    })

    # ----- ADC carry-chain idioms (Phase B #7, 2026-04-25) -----
    # These exercise recomp.py's "ADC #0 after carry chain" branch
    # at _emit_adc — the path that fires when self.carry_chain is
    # set by a prior ADC. Single-instruction fuzz can't reach it
    # (carry_chain starts None each snippet). Closing this gap is
    # what would have caught the koopa-stomp bug: the carry-fold
    # emit forgot to wrap result in (a_type), so C int promotion
    # made $FF + $01 = 256 instead of 0, Z flag wrong, BNE wrong.
    #
    # Each snippet ends with the standard fuzz epilogue's Z capture
    # at $1F07. If recomp's flag_src tracks an un-truncated int, Z
    # will diverge from snes9x.

    # 1. Minimal carry-fold reproducer: A=$FF, carry-in via ADC #$80
    #    overflow. Post-ADC #0: A=$00 (wraps), Z=1, C=1.
    rom = bytearray()
    rom += bytes([0xC2, 0x30])           # REP #$30 (16-bit init)
    rom += bytes([0xE2, 0x30])           # SEP #$30 (back to 8-bit M+X)
    rom += bytes([0xA2, 0x00])           # LDX #0
    rom += bytes([0xA0, 0x00])           # LDY #0
    rom += bytes([0x18])                 # CLC
    rom += bytes([0xB8])                 # CLV
    rom += bytes([0xA9, 0xF4])           # LDA #$F4
    rom += bytes([0x69, 0x80])           # ADC #$80 (sets carry_chain, C=1)
    rom += bytes([0xA9, 0xFF])           # LDA #$FF (carry_chain still set)
    rom += bytes([0x69, 0x00])           # ADC #$00 (carry-fold branch)
    out.append({
        'id': 'compound_ADC_carry_fold_FF_to_zero',
        'opcode': 0x69, 'mnem': 'ADC', 'mode': 'IMM',
        'm_flag': 1, 'x_flag': 1,
        'seed_name': 'ff_carry_in',
        'seed': {'A': 0xF4, 'X': 0, 'Y': 0},
        'rom_hex': bytes(rom).hex(),
        'touches_A': True, 'touches_X': False, 'touches_Y': False,
        'reads_mem': False, 'writes_mem': False,
        '_compound': True,
    })

    # 2. Carry-fold with A=$00, carry-in=1. Result: A=$01, Z=0, C=0.
    #    Sanity: confirm the carry-fold path produces the right value
    #    when the wrap-to-zero is NOT involved.
    rom = bytearray()
    rom += bytes([0xC2, 0x30])
    rom += bytes([0xE2, 0x30])
    rom += bytes([0xA2, 0x00])
    rom += bytes([0xA0, 0x00])
    rom += bytes([0x18])                 # CLC
    rom += bytes([0xB8])                 # CLV
    rom += bytes([0xA9, 0xF4])           # LDA #$F4
    rom += bytes([0x69, 0x80])           # ADC #$80 (C=1)
    rom += bytes([0xA9, 0x00])           # LDA #$00
    rom += bytes([0x69, 0x00])           # ADC #$00 → A=$01
    out.append({
        'id': 'compound_ADC_carry_fold_zero_plus_carry',
        'opcode': 0x69, 'mnem': 'ADC', 'mode': 'IMM',
        'm_flag': 1, 'x_flag': 1,
        'seed_name': 'zero_carry_in',
        'seed': {'A': 0xF4, 'X': 0, 'Y': 0},
        'rom_hex': bytes(rom).hex(),
        'touches_A': True, 'touches_X': False, 'touches_Y': False,
        'reads_mem': False, 'writes_mem': False,
        '_compound': True,
    })

    # 3. Carry-fold with NO carry-in (ADC #$80 didn't overflow).
    #    A=$FF, no carry. Result: A=$FF, Z=0, C=0.
    rom = bytearray()
    rom += bytes([0xC2, 0x30])
    rom += bytes([0xE2, 0x30])
    rom += bytes([0xA2, 0x00])
    rom += bytes([0xA0, 0x00])
    rom += bytes([0x18])                 # CLC
    rom += bytes([0xB8])                 # CLV
    rom += bytes([0xA9, 0x40])           # LDA #$40
    rom += bytes([0x69, 0x10])           # ADC #$10 → A=$50, C=0 (no overflow)
    rom += bytes([0xA9, 0xFF])           # LDA #$FF
    rom += bytes([0x69, 0x00])           # ADC #$00 → A=$FF, C=0
    out.append({
        'id': 'compound_ADC_carry_fold_no_overflow',
        'opcode': 0x69, 'mnem': 'ADC', 'mode': 'IMM',
        'm_flag': 1, 'x_flag': 1,
        'seed_name': 'no_overflow',
        'seed': {'A': 0x40, 'X': 0, 'Y': 0},
        'rom_hex': bytes(rom).hex(),
        'touches_A': True, 'touches_X': False, 'touches_Y': False,
        'reads_mem': False, 'writes_mem': False,
        '_compound': True,
    })

    # 4. Full CheckForContact-style SBC + carry-fold idiom.
    #    Inputs: low Mario=$57, low Sprite=$63 (Mario above sprite, lo
    #    underflow); high Mario=$01, high Sprite=$01 (high underflow due
    #    to lo borrow). Per ROM, this is the f269 koopa-stomp moment.
    #    Expected: post-carry-fold A=$00, Z=1, C=1 (contact detected).
    rom = bytearray()
    rom += bytes([0xC2, 0x30])
    rom += bytes([0xE2, 0x30])
    rom += bytes([0xA2, 0x00])
    rom += bytes([0xA0, 0x00])
    rom += bytes([0x18])
    rom += bytes([0xB8])
    rom += bytes([0x38])                 # SEC (SBC needs C=1)
    rom += bytes([0xA9, 0x57])           # LDA #$57 (Mario Y lo)
    rom += bytes([0xE9, 0x63])           # SBC #$63 → A=$F4, C=0 (borrow)
    rom += bytes([0x48])                 # PHA
    rom += bytes([0xA9, 0x01])           # LDA #$01 (Mario Y hi)
    rom += bytes([0xE9, 0x01])           # SBC #$01 → A=$01-$01-1=$FF, C=0
    rom += bytes([0x85, 0x0C])           # STA $0C ($C = $FF)
    rom += bytes([0x68])                 # PLA → A=$F4
    rom += bytes([0x18])                 # CLC
    rom += bytes([0x69, 0x80])           # ADC #$80 → A=$74, C=1 (overflow)
    rom += bytes([0xA5, 0x0C])           # LDA $0C → A=$FF
    rom += bytes([0x69, 0x00])           # ADC #$00 → A=$00, Z=1, C=1
    out.append({
        'id': 'compound_CheckForContact_carry_fold_idiom',
        'opcode': 0x69, 'mnem': 'ADC', 'mode': 'IMM',
        'm_flag': 1, 'x_flag': 1,
        'seed_name': 'check_for_contact',
        'seed': {'A': 0, 'X': 0, 'Y': 0},
        'rom_hex': bytes(rom).hex(),
        'touches_A': True, 'touches_X': False, 'touches_Y': False,
        'reads_mem': False, 'writes_mem': True,
        '_compound': True,
    })

    # SEP narrows A for subsequent 8-bit CMP. Test that the CMP
    # uses only A's low byte. End state: A = $CD (low byte of $ABCD),
    # CMP #$CD sets Z=1 (and C=1 since equal). M=1, X=0.
    rom = bytearray()
    rom += bytes([0xC2, 0x30])
    rom += bytes([0xA9, 0xCD, 0xAB])     # LDA #$ABCD (3-byte at M=0)
    rom += bytes([0xA2, 0x00, 0x00])     # LDX #0 (X=16-bit)
    rom += bytes([0xA0, 0x00, 0x00])
    rom += bytes([0x18])
    rom += bytes([0xB8])
    rom += bytes([0xE2, 0x20])           # SEP #$20 — A -> 8-bit ($CD)
    rom += bytes([0xC9, 0xCD])           # CMP #$CD (8-bit imm; sets Z=1, C=1)
    out.append({
        'id': 'compound_SEP_narrows_A_for_CMP',
        'opcode': 0xC9, 'mnem': 'CMP', 'mode': 'IMM',
        'm_flag': 1, 'x_flag': 0,
        'seed_name': 'sep_narrow',
        'seed': {'A': 0xABCD, 'X': 0, 'Y': 0},
        'rom_hex': bytes(rom).hex(),
        'touches_A': False, 'touches_X': False, 'touches_Y': False,
        'reads_mem': False, 'writes_mem': False,
        '_compound': True,
    })

    # ----- Multi-insn flag-state propagation (Phase B #7+, 2026-04-26) -----
    #
    # These exercise recomp.py's cross-instruction state: self.overflow,
    # self.flag_src, self.carry. Single-insn fuzz can't reach these
    # branches because they read state set by a PRIOR instruction.
    #
    # Each snippet ends with the standard flag-capture epilogue, so a
    # mismatch shows up as a differing $1F06–$1F09 byte (C/Z/N/V).

    # 1. ADC overflow → V flag must propagate. Two positives ($60 + $20
    #    = $80) overflow signed. Recomp's _emit_adc must set self.overflow
    #    so the BVC capture in the epilogue reads V=1.
    rom = bytearray()
    rom += bytes([0xC2, 0x30])
    rom += bytes([0xE2, 0x30])
    rom += bytes([0xA2, 0x00, 0xA0, 0x00, 0x18, 0xB8])
    rom += bytes([0xA9, 0x60])           # LDA #$60
    rom += bytes([0x69, 0x20])           # ADC #$20 → A=$80, V=1 (signed overflow)
    out.append({
        'id': 'compound_ADC_overflow_sets_V',
        'opcode': 0x69, 'mnem': 'ADC', 'mode': 'IMM',
        'm_flag': 1, 'x_flag': 1, 'seed_name': 'pos_pos_overflow',
        'seed': {'A': 0x60, 'X': 0, 'Y': 0},
        'rom_hex': bytes(rom).hex(),
        'touches_A': True, 'touches_X': False, 'touches_Y': False,
        'reads_mem': False, 'writes_mem': False, '_compound': True,
    })

    # 2. ADC no-overflow → V must be CLEAR. $10 + $20 = $30 (no overflow).
    rom = bytearray()
    rom += bytes([0xC2, 0x30])
    rom += bytes([0xE2, 0x30])
    rom += bytes([0xA2, 0x00, 0xA0, 0x00, 0x18, 0xB8])
    rom += bytes([0xA9, 0x10])
    rom += bytes([0x69, 0x20])           # ADC #$20 → A=$30, V=0
    out.append({
        'id': 'compound_ADC_no_overflow_clears_V',
        'opcode': 0x69, 'mnem': 'ADC', 'mode': 'IMM',
        'm_flag': 1, 'x_flag': 1, 'seed_name': 'small_no_overflow',
        'seed': {'A': 0x10, 'X': 0, 'Y': 0},
        'rom_hex': bytes(rom).hex(),
        'touches_A': True, 'touches_X': False, 'touches_Y': False,
        'reads_mem': False, 'writes_mem': False, '_compound': True,
    })

    # 3. SBC overflow → V flag. $80 - $01 = $7F (signed: -128 - 1 = +127,
    #    overflow because true result -129 doesn't fit in 8-bit signed).
    rom = bytearray()
    rom += bytes([0xC2, 0x30])
    rom += bytes([0xE2, 0x30])
    rom += bytes([0xA2, 0x00, 0xA0, 0x00, 0xB8])
    rom += bytes([0x38])                 # SEC (SBC needs C=1 for normal sub)
    rom += bytes([0xA9, 0x80])
    rom += bytes([0xE9, 0x01])           # SBC #$01 → A=$7F, V=1
    out.append({
        'id': 'compound_SBC_overflow_sets_V',
        'opcode': 0xE9, 'mnem': 'SBC', 'mode': 'IMM',
        'm_flag': 1, 'x_flag': 1, 'seed_name': 'sbc_signed_overflow',
        'seed': {'A': 0x80, 'X': 0, 'Y': 0},
        'rom_hex': bytes(rom).hex(),
        'touches_A': True, 'touches_X': False, 'touches_Y': False,
        'reads_mem': False, 'writes_mem': False, '_compound': True,
    })

    # 4. BIT abs → V from bit 6 of memory (NOT from A&mem). $40 has
    #    bit 6 set → V=1 regardless of A. Use ABS so the BIT actually
    #    reads memory (BIT IMM doesn't set V). Pre-write $40 to $0100.
    rom = bytearray()
    rom += bytes([0xC2, 0x30])
    rom += bytes([0xE2, 0x30])
    rom += bytes([0xA2, 0x00, 0xA0, 0x00, 0x18, 0xB8])
    rom += bytes([0xA9, 0x40])           # LDA #$40
    rom += bytes([0x8D, 0x00, 0x01])     # STA $0100 — set memory to $40
    rom += bytes([0xA9, 0xFF])           # LDA #$FF (so A&mem != 0)
    rom += bytes([0x2C, 0x00, 0x01])     # BIT $0100 → V=1 (bit 6 of mem)
    out.append({
        'id': 'compound_BIT_abs_V_from_mem_bit6',
        'opcode': 0x2C, 'mnem': 'BIT', 'mode': 'ABS',
        'm_flag': 1, 'x_flag': 1, 'seed_name': 'bit6_set',
        'seed': {'A': 0xFF, 'X': 0, 'Y': 0},
        'rom_hex': bytes(rom).hex(),
        'touches_A': False, 'touches_X': False, 'touches_Y': False,
        'reads_mem': True, 'writes_mem': True, '_compound': True,
    })

    # 5. CMP equal → Z=1, C=1. Standard CMP semantics; recomp must
    #    emit the post-CMP flag tracking that the BNE/BCC capture reads.
    rom = bytearray()
    rom += bytes([0xC2, 0x30])
    rom += bytes([0xE2, 0x30])
    rom += bytes([0xA2, 0x00, 0xA0, 0x00, 0x18, 0xB8])
    rom += bytes([0xA9, 0x55])
    rom += bytes([0xC9, 0x55])           # CMP #$55 → Z=1, C=1, N=0
    out.append({
        'id': 'compound_CMP_equal_sets_Z_C',
        'opcode': 0xC9, 'mnem': 'CMP', 'mode': 'IMM',
        'm_flag': 1, 'x_flag': 1, 'seed_name': 'cmp_equal',
        'seed': {'A': 0x55, 'X': 0, 'Y': 0},
        'rom_hex': bytes(rom).hex(),
        'touches_A': False, 'touches_X': False, 'touches_Y': False,
        'reads_mem': False, 'writes_mem': False, '_compound': True,
    })

    # 6. CMP A<mem → C=0 (borrow), Z=0. $20 < $80 → A-mem = $20-$80 =
    #    underflow. Tests carry semantics for less-than.
    rom = bytearray()
    rom += bytes([0xC2, 0x30])
    rom += bytes([0xE2, 0x30])
    rom += bytes([0xA2, 0x00, 0xA0, 0x00, 0x18, 0xB8])
    rom += bytes([0xA9, 0x20])
    rom += bytes([0xC9, 0x80])           # CMP #$80 → C=0 (A<mem), N=1, Z=0
    out.append({
        'id': 'compound_CMP_A_less_clears_C',
        'opcode': 0xC9, 'mnem': 'CMP', 'mode': 'IMM',
        'm_flag': 1, 'x_flag': 1, 'seed_name': 'cmp_a_less',
        'seed': {'A': 0x20, 'X': 0, 'Y': 0},
        'rom_hex': bytes(rom).hex(),
        'touches_A': False, 'touches_X': False, 'touches_Y': False,
        'reads_mem': False, 'writes_mem': False, '_compound': True,
    })

    # 7. CMP A>=mem → C=1, Z=0. $80 >= $20 → C=1.
    rom = bytearray()
    rom += bytes([0xC2, 0x30])
    rom += bytes([0xE2, 0x30])
    rom += bytes([0xA2, 0x00, 0xA0, 0x00, 0x18, 0xB8])
    rom += bytes([0xA9, 0x80])
    rom += bytes([0xC9, 0x20])           # CMP #$20 → C=1, Z=0, N=0
    out.append({
        'id': 'compound_CMP_A_greater_sets_C',
        'opcode': 0xC9, 'mnem': 'CMP', 'mode': 'IMM',
        'm_flag': 1, 'x_flag': 1, 'seed_name': 'cmp_a_greater',
        'seed': {'A': 0x80, 'X': 0, 'Y': 0},
        'rom_hex': bytes(rom).hex(),
        'touches_A': False, 'touches_X': False, 'touches_Y': False,
        'reads_mem': False, 'writes_mem': False, '_compound': True,
    })

    # 8. INC dp → flag_src from RMW result. Pre-write $7F to $10, then
    #    INC $10 → mem becomes $80, N=1 (bit 7 set in result), Z=0.
    rom = bytearray()
    rom += bytes([0xC2, 0x30])
    rom += bytes([0xE2, 0x30])
    rom += bytes([0xA2, 0x00, 0xA0, 0x00, 0x18, 0xB8])
    rom += bytes([0xA9, 0x7F])
    rom += bytes([0x85, 0x10])           # STA $10 (mem=$7F)
    rom += bytes([0xE6, 0x10])           # INC $10 → mem=$80, N=1
    out.append({
        'id': 'compound_INC_dp_N_from_result',
        'opcode': 0xE6, 'mnem': 'INC', 'mode': 'DP',
        'm_flag': 1, 'x_flag': 1, 'seed_name': 'inc_to_negative',
        'seed': {'A': 0x7F, 'X': 0, 'Y': 0},
        'rom_hex': bytes(rom).hex(),
        'touches_A': False, 'touches_X': False, 'touches_Y': False,
        'reads_mem': True, 'writes_mem': True, '_compound': True,
    })

    # 9. ASL A → C from shift-out (bit 7 → C). A=$81 → A=$02, C=1, N=0.
    rom = bytearray()
    rom += bytes([0xC2, 0x30])
    rom += bytes([0xE2, 0x30])
    rom += bytes([0xA2, 0x00, 0xA0, 0x00, 0x18, 0xB8])
    rom += bytes([0xA9, 0x81])
    rom += bytes([0x0A])                 # ASL A → A=$02, C=1
    out.append({
        'id': 'compound_ASL_A_C_from_bit7',
        'opcode': 0x0A, 'mnem': 'ASL', 'mode': 'ACC',
        'm_flag': 1, 'x_flag': 1, 'seed_name': 'asl_carry_out',
        'seed': {'A': 0x81, 'X': 0, 'Y': 0},
        'rom_hex': bytes(rom).hex(),
        'touches_A': True, 'touches_X': False, 'touches_Y': False,
        'reads_mem': False, 'writes_mem': False, '_compound': True,
    })

    # 10. SBC borrow chain (lo + hi) — classic 16-bit-via-8-bit-pair
    #     subtraction. $0100 - $0001 = $00FF. After the chain, hi byte
    #     ($0C) holds $00, lo holds $FF. Tests carry-out from lo SBC
    #     feeding carry-in of hi SBC.
    rom = bytearray()
    rom += bytes([0xC2, 0x30])
    rom += bytes([0xE2, 0x30])
    rom += bytes([0xA2, 0x00, 0xA0, 0x00, 0xB8])
    rom += bytes([0x38])                 # SEC
    rom += bytes([0xA9, 0x00])           # LDA #$00 (lo of $0100)
    rom += bytes([0xE9, 0x01])           # SBC #$01 → A=$FF, C=0 (borrow)
    rom += bytes([0x85, 0x10])           # STA $10 (lo result)
    rom += bytes([0xA9, 0x01])           # LDA #$01 (hi of $0100)
    rom += bytes([0xE9, 0x00])           # SBC #$00 → with C=0: A=$01-$00-1=$00, C=1
    rom += bytes([0x85, 0x0C])           # STA $0C (hi result)
    out.append({
        'id': 'compound_SBC_borrow_chain_lo_hi',
        'opcode': 0xE9, 'mnem': 'SBC', 'mode': 'IMM',
        'm_flag': 1, 'x_flag': 1, 'seed_name': 'borrow_chain',
        'seed': {'A': 0, 'X': 0, 'Y': 0},
        'rom_hex': bytes(rom).hex(),
        'touches_A': True, 'touches_X': False, 'touches_Y': False,
        'reads_mem': False, 'writes_mem': True, '_compound': True,
    })

    # 11. REP widens A then CMP at M=0. CMP #$1234 against A=$1234 →
    #     Z=1, C=1 — but only if recomp uses 16-bit imm and 16-bit A.
    #     If recomp lost the M=0 transition, CMP would compare $34 vs
    #     $34 (Z=1) but consume only 1 byte of imm and the next $12
    #     would be decoded as the next opcode → divergence.
    rom = bytearray()
    rom += bytes([0xE2, 0x30])           # SEP #$30 (start 8-bit)
    rom += bytes([0xA2, 0x00, 0xA0, 0x00, 0x18, 0xB8])
    rom += bytes([0xC2, 0x20])           # REP #$20 (A → 16-bit)
    rom += bytes([0xA9, 0x34, 0x12])     # LDA #$1234 (now 3-byte)
    rom += bytes([0xC9, 0x34, 0x12])     # CMP #$1234 → Z=1, C=1
    out.append({
        'id': 'compound_REP_widens_A_for_CMP',
        'opcode': 0xC9, 'mnem': 'CMP', 'mode': 'IMM',
        'm_flag': 0, 'x_flag': 1, 'seed_name': 'rep_then_cmp',
        'seed': {'A': 0, 'X': 0, 'Y': 0},
        'rom_hex': bytes(rom).hex(),
        'touches_A': False, 'touches_X': False, 'touches_Y': False,
        'reads_mem': False, 'writes_mem': False, '_compound': True,
    })

    # 12. LSR A then BCC: shift-out bit 0 → C. A=$01 → A=$00, C=1, Z=1.
    #     The BCC in the epilogue should NOT branch (C set), so $1F06
    #     captures C=set (slot=0).
    rom = bytearray()
    rom += bytes([0xC2, 0x30])
    rom += bytes([0xE2, 0x30])
    rom += bytes([0xA2, 0x00, 0xA0, 0x00, 0x18, 0xB8])
    rom += bytes([0xA9, 0x01])
    rom += bytes([0x4A])                 # LSR A → A=$00, C=1, Z=1
    out.append({
        'id': 'compound_LSR_A_C_from_bit0_with_Z',
        'opcode': 0x4A, 'mnem': 'LSR', 'mode': 'ACC',
        'm_flag': 1, 'x_flag': 1, 'seed_name': 'lsr_to_zero',
        'seed': {'A': 0x01, 'X': 0, 'Y': 0},
        'rom_hex': bytes(rom).hex(),
        'touches_A': True, 'touches_X': False, 'touches_Y': False,
        'reads_mem': False, 'writes_mem': False, '_compound': True,
    })

    # 13. ROL A carries into bit 0 from C. C=1 + A=$80 → A=$01, C=1.
    #     Tests both the C-in (rotate input) and C-out (shift output).
    rom = bytearray()
    rom += bytes([0xC2, 0x30])
    rom += bytes([0xE2, 0x30])
    rom += bytes([0xA2, 0x00, 0xA0, 0x00, 0xB8])
    rom += bytes([0x38])                 # SEC (C=1 for ROL input)
    rom += bytes([0xA9, 0x80])
    rom += bytes([0x2A])                 # ROL A → A=$01, C=1 (was bit7)
    out.append({
        'id': 'compound_ROL_A_carry_chain',
        'opcode': 0x2A, 'mnem': 'ROL', 'mode': 'ACC',
        'm_flag': 1, 'x_flag': 1, 'seed_name': 'rol_carry_through',
        'seed': {'A': 0x80, 'X': 0, 'Y': 0},
        'rom_hex': bytes(rom).hex(),
        'touches_A': True, 'touches_X': False, 'touches_Y': False,
        'reads_mem': False, 'writes_mem': False, '_compound': True,
    })

    return out


def main():
    OUTPUT_DIR.mkdir(exist_ok=True)

    with open(OPCODE_TABLE) as f:
        table = json.load(f)

    snippets = []
    for e in table:
        if not in_scope(e):
            continue
        for m_flag in (0, 1):
            # Skip m_flag=0 runs for opcodes where M doesn't matter (IMP).
            if e['mode'] == 'IMP' and not e['m_dep']:
                if m_flag == 0:
                    continue
            for x_flag in (0, 1):
                if e['mode'] == 'IMP' and not e['x_dep']:
                    if x_flag == 0:
                        continue
                # Skip M=1 path for M-dep IMM opcodes if we already covered M=0?
                # No — we want BOTH, that's the point.
                for seed_name, seed in seeds_for(e['mnem'], e['mode'], m_flag, x_flag):
                    rom = prologue(m_flag, x_flag, seed)
                    try:
                        rom += encode_insn(e['opcode'], e['mode'], m_flag, x_flag, e['mnem'])
                    except ValueError as exc:
                        continue
                    rom += epilogue(m_flag, x_flag)
                    rom += bytes([0x60])  # RTS
                    snip = {
                        'id': f'{e["mnem"]}_{e["mode"]}_M{m_flag}_X{x_flag}_{seed_name}_op{e["opcode"]:02x}',
                        'opcode': e['opcode'],
                        'mnem': e['mnem'],
                        'mode': e['mode'],
                        'm_flag': m_flag,
                        'x_flag': x_flag,
                        'seed_name': seed_name,
                        'seed': seed,
                        'rom_hex': rom.hex(),
                        'touches_A': e['touches_A'],
                        'touches_X': e['touches_X'],
                        'touches_Y': e['touches_Y'],
                        'reads_mem': e['reads_mem'],
                        'writes_mem': e['writes_mem'],
                    }
                    snippets.append(snip)

    # Phase B #6: compound multi-instruction snippets. Each carries
    # its own pre-built ROM (prologue + test sequence); we only
    # append the standard epilogue + RTS here.
    for snip in compound_snippets():
        rom = bytes.fromhex(snip['rom_hex'])
        # Determine post-test M/X for the epilogue. Decode the snippet
        # to find the final flag state — but compound snippets carry
        # their own m_flag/x_flag declared values, so trust those.
        rom += epilogue(snip['m_flag'], snip['x_flag'])
        rom += bytes([0x60])
        snip['rom_hex'] = rom.hex()
        snippets.append(snip)

    out_path = OUTPUT_DIR / 'snippets.json'
    with open(out_path, 'w') as f:
        json.dump(snippets, f, indent=1)
    print(f'wrote {out_path} — {len(snippets)} snippets')

    # Distribution report
    by_mnem = {}
    for s in snippets:
        by_mnem.setdefault(s['mnem'], 0)
        by_mnem[s['mnem']] += 1
    for mn in sorted(by_mnem):
        print(f'  {mn:4s}: {by_mnem[mn]}')


if __name__ == '__main__':
    main()
