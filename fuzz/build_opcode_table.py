"""Emit opcode_table.json for Phase B differential fuzzing.

Derived from the project's own decoder (snes65816.py) so the fuzz
harness and the emitter stay in lockstep: if the decoder recognizes
an opcode, the fuzzer will try to exercise it. If the decoder
doesn't, we won't invent a fake description.

Per-entry fields:
  opcode        (0-255)
  mnem          ('LDA', 'ASL', ...)
  mode          ('IMM', 'DP', 'ABS_X', ...)
  m_dep         True iff operand size depends on M flag (IMM-mode LDA etc.)
  x_dep         True iff operand size depends on X flag (IMM-mode LDX etc.)
  length        if fixed, the instruction byte count; 'variable' otherwise
  touches_A     derived from mnem — does A change after execution
  touches_X     derived from mnem
  touches_Y     derived from mnem
  reads_mem     True iff mnem reads memory in any mode
  writes_mem    True iff mnem writes memory
  flags_touched subset of {N,V,Z,C,D,I,M,X,E} the opcode may modify

Run once; re-run after decoder table changes.
"""
from __future__ import annotations
import json
import pathlib
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / 'recompiler'))

import snes65816 as s65  # noqa: E402


# ----- mnemonic classification ------------------------------------------------
# These are derived by hand from the 65816 ISA. Each is an invariant about
# the mnemonic, not the opcode: e.g. all LDA variants touch A regardless of mode.

TOUCHES_A = {
    'LDA', 'STA',  # STA doesn't *modify* A but belongs here so we snapshot pre/post
    'TXA', 'TYA', 'TAX', 'TAY',  # TAX/TAY don't modify A but we want A observed
    'PLA', 'PHA',                # PHA reads A; PLA writes A
    'ADC', 'SBC', 'AND', 'ORA', 'EOR',
    'ASL', 'LSR', 'ROL', 'ROR', 'INC', 'DEC',  # ACC mode
    'XBA', 'CMP', 'BIT',
    'TCD', 'TDC', 'TCS', 'TSC',
}

TOUCHES_X = {
    'LDX', 'STX', 'TAX', 'TXA', 'TSX', 'TXS', 'TXY', 'TYX',
    'INX', 'DEX', 'CPX', 'PHX', 'PLX',
}

TOUCHES_Y = {
    'LDY', 'STY', 'TAY', 'TYA', 'TXY', 'TYX',
    'INY', 'DEY', 'CPY', 'PHY', 'PLY',
}

# Any mnemonic that in any of its modes reads memory. Opcodes like TAX/INX
# that never read memory are excluded.
READS_MEM = {
    'LDA', 'LDX', 'LDY',
    'AND', 'ORA', 'EOR', 'ADC', 'SBC', 'CMP', 'CPX', 'CPY', 'BIT',
    'ASL', 'LSR', 'ROL', 'ROR', 'INC', 'DEC',  # memory variants
    'TSB', 'TRB',
    'PLA', 'PLX', 'PLY', 'PLP', 'PLB', 'PLD',
    'MVN', 'MVP',
    'JMP', 'JSR', 'JSL', 'RTS', 'RTL', 'RTI',
}

WRITES_MEM = {
    'STA', 'STX', 'STY', 'STZ',
    'ASL', 'LSR', 'ROL', 'ROR', 'INC', 'DEC',
    'TSB', 'TRB',
    'PHA', 'PHX', 'PHY', 'PHP', 'PHB', 'PHD', 'PHK',
    'PEA', 'PEI', 'PER',
    'JSR', 'JSL', 'BRK', 'COP',
    'MVN', 'MVP',
}

# Flags touched per mnemonic. Union of all addressing modes.
FLAGS_N_Z = {'LDA','LDX','LDY','TAX','TAY','TXA','TYA','TXY','TYX','TSX',
             'INX','INY','DEX','DEY','INC','DEC','AND','ORA','EOR',
             'ASL','LSR','ROL','ROR','BIT','PLA','PLX','PLY','XBA','TCD','TDC',
             'TSB','TRB'}  # Z only for TSB/TRB
FLAGS_C    = {'ADC','SBC','CMP','CPX','CPY','ASL','LSR','ROL','ROR',
              'CLC','SEC','XCE'}
FLAGS_V    = {'ADC','SBC','BIT','CLV'}
FLAGS_D    = {'CLD','SED'}
FLAGS_I    = {'CLI','SEI'}
FLAGS_M_X  = {'REP','SEP','PLP','XCE'}  # REP/SEP modify M/X directly


def mnem_flags(mnem: str) -> list[str]:
    out = []
    if mnem in FLAGS_N_Z: out += ['N','Z']
    if mnem in FLAGS_C:   out.append('C')
    if mnem in FLAGS_V:   out.append('V')
    if mnem in FLAGS_D:   out.append('D')
    if mnem in FLAGS_I:   out.append('I')
    if mnem in FLAGS_M_X:
        out += ['M','X']
    # CMP/CPX/CPY also set N and Z
    if mnem in ('CMP','CPX','CPY') and 'N' not in out:
        out += ['N','Z']
    # ADC/SBC set N/Z/C/V
    if mnem in ('ADC','SBC'):
        for f in ('N','Z'):
            if f not in out: out.append(f)
    # PLP/RTI restore all flags
    if mnem in ('PLP','RTI'):
        out = ['N','V','M','X','D','I','Z','C']
    return sorted(set(out))


# ----- mode name lookup -------------------------------------------------------

MODE_STR = {
    s65.IMP: 'IMP',
    s65.ACC: 'ACC',
    s65.IMM: 'IMM',
    s65.DP: 'DP',
    s65.DP_X: 'DP_X',
    s65.DP_Y: 'DP_Y',
    s65.ABS: 'ABS',
    s65.ABS_X: 'ABS_X',
    s65.ABS_Y: 'ABS_Y',
    s65.LONG: 'LONG',
    s65.LONG_X: 'LONG_X',
    s65.REL: 'REL',
    s65.REL16: 'REL16',
    s65.STK: 'STK',
    s65.INDIR: 'INDIR',
    s65.INDIR_X: 'INDIR_X',
    s65.INDIR_Y: 'INDIR_Y',
    s65.INDIR_LY: 'INDIR_LY',
    s65.INDIR_L: 'INDIR_L',
    s65.INDIR_DPX: 'INDIR_DPX',
    s65.DP_INDIR: 'DP_INDIR',
    s65.STK_IY: 'STK_IY',
}


def main():
    out = []
    for op in range(256):
        if op not in s65._OPCODES:
            continue
        mnem, mode, length = s65._OPCODES[op]
        m_dep = callable(length)
        x_dep = False
        fixed_len = None
        if m_dep:
            # Probe: (length at M=1) vs (length at M=0). If they differ, this is
            # an M-dep IMM opcode; otherwise fall through to X-dep below.
            len_m1 = length(1, 1)
            len_m0 = length(0, 1)
            if len_m1 != len_m0:
                m_dep = True
                x_dep = False
            else:
                # Might be X-dep
                len_x1 = length(1, 1)
                len_x0 = length(1, 0)
                if len_x1 != len_x0:
                    m_dep = False
                    x_dep = True
                else:
                    m_dep = False
                    x_dep = False
                    fixed_len = len_m1
        else:
            fixed_len = length

        entry = {
            'opcode':     op,
            'mnem':       mnem,
            'mode':       MODE_STR.get(mode, f'UNKNOWN_{mode}'),
            'm_dep':      m_dep,
            'x_dep':      x_dep,
            'length':     fixed_len if fixed_len is not None else 'variable',
            'touches_A':  mnem in TOUCHES_A,
            'touches_X':  mnem in TOUCHES_X,
            'touches_Y':  mnem in TOUCHES_Y,
            'reads_mem':  mnem in READS_MEM,
            'writes_mem': mnem in WRITES_MEM,
            'flags':      mnem_flags(mnem),
        }
        out.append(entry)

    out_path = pathlib.Path(__file__).parent / 'opcode_table.json'
    with open(out_path, 'w') as f:
        json.dump(out, f, indent=2)
    print(f'wrote {out_path} — {len(out)} opcodes')

    # Sanity report
    by_mnem = {}
    for e in out:
        by_mnem.setdefault(e['mnem'], []).append(e['mode'])
    for mn in sorted(by_mnem):
        modes = sorted(set(by_mnem[mn]))
        print(f'  {mn:4s} : {len(by_mnem[mn])} opcodes across {modes}')


if __name__ == '__main__':
    main()
