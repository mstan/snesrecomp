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
    # Others worth covering up front.
    'XBA', 'CLC', 'SEC',
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
