#!/usr/bin/env python3
"""
recompiler/snes_cycles.py -- Authoritative 65816 / SNES cycle cost model.

This is the SINGLE SOURCE OF TRUTH for cycle accounting, consumed by both
backends (Axis 2 of SNES_ACCURACY_BURNDOWN.md):

  * the v2 recompiler emitter (gen time): folds a block's instructions to a
    per-block integer constant of CPU cycles (near-free; stays fast);
  * the dev-only reference engine (interp816, when reconciled onto this
    branch): evaluates the same model at runtime.

To keep the C side (runtime / reference engine) from ever drifting from this
Python authority, run ``python recompiler/snes_cycles.py --emit-c
runner/src/snes/snes_cycles.h``: it bakes the per-opcode static contribution
tables (base, m-add, x-add, dp-add, page-cross, branch class, e-class) into a
generated header whose inline combiner mirrors ``instr_cpu_cycles`` below.
The arrays are the source of truth; the combination logic is small and is
asserted equivalent by tests/test_snes_cycles.py.

================================================================================
PROVENANCE (owner directive 2026-06-27: datasheet-checked, not invented)
================================================================================

Two layers, two grounded references, cross-checked:

  LAYER 1 - 65816 CPU (bus) cycles per instruction
    Source: undisbeliever, "65816 Reference: Opcodes"
            https://undisbeliever.net/snesdev/65816-opcodes.html
    Base cycle counts assume the minimum case: m=1 (8-bit A/mem), x=1 (8-bit
    index), Direct-Page register low byte D.l == 0, no index page-cross,
    conditional branch NOT taken, native mode (e=0) except where an e-modifier
    is noted. Documented conditional modifiers:
      +1 if m=0      16-bit accumulator/memory access (loads/stores/ALU/STZ/BIT)
      +2 if m=0      read-modify-write on memory (ASL/LSR/ROL/ROR/INC/DEC/TSB/TRB)
      +1 if x=0      16-bit index op (LDX/LDY/STX/STY/CPX/CPY, PHX/PHY/PLX/PLY)
      +1 if D.l != 0 any Direct-Page-based addressing mode (and PEI)
      +1 if x-cross  read op whose abs,X / abs,Y / (dp),Y effective addr crosses
                     a 256-byte page (NOT stores, NOT long,X)
      +1 if taken    conditional branch taken
      +1 more        taken branch that crosses a page, EMULATION mode only (e=1)
      +1 if e=0      RTI / BRK / COP in native mode

  LAYER 2 - SNES master-clock cost per memory access ("speed map")
    Source: undisbeliever 65816 page + fullsnes (problemkaputt.de/fullsnes.htm),
            cross-checked against snes.nesdev.org/wiki/Timing for the 12-cycle
            cell (the fullsnes auto-summary mis-stated $4000-$41FF as 8; the
            nesdev wiki and SFC dev wiki confirm 12 / "XSlow").
    Master clock = 21.47727 MHz. One access costs 6, 8, or 12 master clocks:
      6  (fast,  3.58 MHz): $00-$3F:$2000-$3FFF, $00-$3F:$4200-$5FFF,
                            and $80-$FF:$8000-$FFFF when FastROM (MEMSEL=1)
      8  (slow,  2.68 MHz): $00-$3F:$0000-$1FFF (WRAM mirror),
                            $00-$3F:$6000-$7FFF, $00-$3F:$8000-$FFFF (LoROM),
                            $40-$7D, $7E-$7F (WRAM),
                            and $80-$FF:$8000-$FFFF when slow (MEMSEL=0)
      12 (xslow):           $00-$3F:$4000-$41FF (manual joypad / internal I/O)
    The $80-$BF:$0000-$7FFF half mirrors $00-$3F:$0000-$7FFF (same speeds).
    Internal CPU cycles (no bus access) are 6 master clocks (fast).

================================================================================
SCOPE NOTE (honest layering, per the completeness directive)
================================================================================
Layer 1 (CPU cycles) and Layer 2 (region speed) are each EXACT and fully
grounded. The combiner ``instr_master_cycles`` that maps an instruction to
master clocks is a documented FIRST-CUT model: it charges the opcode+operand
fetch bytes at the code region's speed and the remaining cycles at the data
region's speed (internal-only cycles at fast/6). Bus-cycle-EXACT attribution
(modelling each individual access in the instruction's bus sequence, as bsnes
does) is the explicit Axis-2 refinement that the cyc_watch harness will
quantify against the bsnes ground-truth hook. The approximation is isolated to
the combiner; nothing here fabricates a "good enough" number silently.
"""

from typing import Optional, Tuple

import snes65816 as _d
from snes65816 import (
    IMP, ACC, IMM, DP, DP_X, DP_Y, ABS, ABS_X, ABS_Y, LONG, LONG_X, REL,
    REL16, STK, INDIR, INDIR_X, INDIR_Y, INDIR_LY, INDIR_L, INDIR_DPX,
    DP_INDIR, STK_IY,
)

# ==============================================================================
# Master clock constants (Layer 2)
# ==============================================================================
MASTER_CLOCK_HZ = 21_477_270
FAST = 6        # master clocks per fast access
SLOW = 8        # per slow access
XSLOW = 12      # per xslow access (manual joypad / internal I/O region)
INTERNAL = FAST  # an internal (non-bus) CPU cycle costs 6 master clocks


def region_speed(addr24: int, memsel: int = 0) -> int:
    """Master clocks for one memory access at 24-bit address `addr24`.

    `memsel` is $420D bit 0 (FastROM): 1 => $80-$FF:$8000-$FFFF runs fast.
    Exact; grounded in the speed map above.
    """
    bank = (addr24 >> 16) & 0xFF
    addr = addr24 & 0xFFFF

    # $40-$7D, $7E-$7F : all slow (HiROM image + WRAM)
    if 0x40 <= bank <= 0x7F:
        return SLOW
    # $C0-$FF : WS2 HiROM image -> fast iff FastROM, else slow
    if bank >= 0xC0:
        return FAST if memsel else SLOW

    # banks $00-$3F and their $80-$BF mirror share the low-half map; only the
    # upper ROM half ($8000-$FFFF) differs (LoROM-slow vs WS2-fast).
    is_system_bank = bank <= 0x3F  # $80-$BF fall through here too

    if addr >= 0x8000:
        if is_system_bank:
            return SLOW                       # $00-$3F:$8000-$FFFF LoROM = slow
        return FAST if memsel else SLOW       # $80-$BF:$8000-$FFFF WS2 LoROM
    # $0000-$7FFF: identical for $00-$3F and $80-$BF mirror
    if addr < 0x2000:
        return SLOW                           # WRAM mirror
    if addr < 0x4000:
        return FAST                           # $2000-$3FFF (PPU/APU/I/O regs)
    if addr < 0x4200:
        return XSLOW                          # $4000-$41FF manual joypad / I/O
    if addr < 0x6000:
        return FAST                           # $4200-$5FFF
    return SLOW                               # $6000-$7FFF expansion


# ==============================================================================
# 65816 CPU-cycle model (Layer 1)
# ==============================================================================

_STORE_MNEMS = {'STA', 'STX', 'STY', 'STZ'}
_RMW_MNEMS = {'ASL', 'LSR', 'ROL', 'ROR', 'INC', 'DEC', 'TSB', 'TRB'}
_M_WIDTH_MNEMS = {'LDA', 'STA', 'ORA', 'AND', 'EOR', 'ADC', 'SBC', 'CMP',
                  'BIT', 'STZ'}
_X_WIDTH_MNEMS = {'LDX', 'LDY', 'STX', 'STY', 'CPX', 'CPY'}
_DP_MODES = {DP, DP_X, DP_Y, DP_INDIR, INDIR_DPX, INDIR_Y, INDIR_L, INDIR_LY}
_XCROSS_MODES = {ABS_X, ABS_Y, INDIR_Y}

_CONDITIONAL_BRANCH_OPS = {0x10, 0x30, 0xF0, 0xD0, 0x90, 0xB0, 0x50, 0x70}
_BRA_OP = 0x80
_E_PLUS1_OPS = {0x40, 0x00, 0x02}   # RTI, BRK, COP : +1 in native mode (e=0)
_BLOCK_MOVE_OPS = {0x44, 0x54}      # MVP, MVN : 7 cycles PER BYTE moved

# Opcodes whose base cycles are fixed and not derivable from a generic
# memory-mode rule (control flow, stack, transfers, flags, specials). Values
# are the minimum base (native, no modifiers).
_SPECIAL_BASE = {
    # jumps / calls / returns
    0x4C: 3,   # JMP abs
    0x5C: 4,   # JML abs long
    0x6C: 5,   # JMP (abs)
    0x7C: 6,   # JMP (abs,X)
    0xDC: 6,   # JML [abs]
    0x20: 6,   # JSR abs
    0xFC: 8,   # JSR (abs,X)
    0x22: 8,   # JSL long
    0x60: 6,   # RTS
    0x6B: 6,   # RTL
    0x40: 6,   # RTI   (+1 if e=0)
    0x00: 7,   # BRK   (+1 if e=0)
    0x02: 7,   # COP   (+1 if e=0)
    # branches
    0x80: 3,   # BRA (always taken)
    0x82: 4,   # BRL
    # stack effective-address pushes
    0xF4: 5,   # PEA
    0xD4: 6,   # PEI  (+1 if D.l != 0)
    0x62: 6,   # PER
    # push / pull
    0x48: 3,   # PHA (+1 if m=0)
    0x68: 4,   # PLA (+1 if m=0)
    0x08: 3,   # PHP
    0x28: 4,   # PLP
    0x8B: 3,   # PHB
    0xAB: 4,   # PLB
    0x0B: 4,   # PHD
    0x2B: 5,   # PLD
    0x4B: 3,   # PHK
    0xDA: 3,   # PHX (+1 if x=0)
    0xFA: 4,   # PLX (+1 if x=0)
    0x5A: 3,   # PHY (+1 if x=0)
    0x7A: 4,   # PLY (+1 if x=0)
    # specials
    0xEB: 3,   # XBA
    0xFB: 2,   # XCE
    0xDB: 3,   # STP
    0xCB: 3,   # WAI
    0xEA: 2,   # NOP
    0x42: 2,   # WDM
    0xC2: 3,   # REP
    0xE2: 3,   # SEP
    # block move (per-byte base; multiply by bytes at runtime)
    0x44: 7,   # MVP
    0x54: 7,   # MVN
}


def _mem_base(mode: int, is_store: bool, is_rmw: bool) -> int:
    """Base CPU cycles for a generic load/store/ALU/RMW memory instruction."""
    if is_rmw:
        # read-modify-write on memory (ACC-mode RMW handled as base 2 above)
        return {DP: 5, DP_X: 6, ABS: 6, ABS_X: 7}[mode]
    fixed = {
        IMM: 2, DP: 3, DP_X: 4, DP_Y: 4, ABS: 4, LONG: 5, LONG_X: 5,
        DP_INDIR: 5, INDIR_DPX: 6, INDIR_L: 6, INDIR_LY: 6, STK: 4, STK_IY: 7,
    }
    if mode in fixed:
        return fixed[mode]
    if mode == ABS_X:
        return 5 if is_store else 4
    if mode == ABS_Y:
        return 5 if is_store else 4
    if mode == INDIR_Y:
        return 6 if is_store else 5
    raise KeyError(f'no base for mode {mode}')


def _info(op: int) -> Optional[Tuple[str, int]]:
    """(mnem, mode) for opcode `op`, or None if undecodable."""
    ent = _d.opcode_table().get(op)
    if ent is None:
        return None
    return ent[0], ent[1]


def base_cpu_cycles(op: int) -> int:
    """Minimum base CPU (bus) cycles for opcode `op`: m=1, x=1, D.l=0, no
    page-cross, conditional branch not taken, native (e=0)."""
    if op in _SPECIAL_BASE:
        return _SPECIAL_BASE[op]
    mn, mode = _info(op)
    if mode in (IMP, ACC):
        return 2                       # transfers, flag ops, ACC shifts/inc/dec
    if mode == REL:
        return 2                       # conditional branch, not taken
    is_store = mn in _STORE_MNEMS
    is_rmw = mn in _RMW_MNEMS
    return _mem_base(mode, is_store, is_rmw)


def m_add(op: int) -> int:
    """Cycles added when m=0 (16-bit accumulator/memory)."""
    if op in (0x48, 0x68):             # PHA / PLA
        return 1
    if op in _SPECIAL_BASE:
        return 0
    mn, mode = _info(op)
    if mode in (IMP, ACC, REL, REL16):
        return 0
    if mn in _RMW_MNEMS:
        return 2                       # RMW reads+writes 16-bit memory
    if mn in _M_WIDTH_MNEMS:
        return 1
    return 0


def x_add(op: int) -> int:
    """Cycles added when x=0 (16-bit index)."""
    if op in (0xDA, 0xFA, 0x5A, 0x7A):  # PHX/PLX/PHY/PLY
        return 1
    if op in _SPECIAL_BASE:
        return 0
    mn, _ = _info(op)
    return 1 if mn in _X_WIDTH_MNEMS else 0


def dp_add(op: int) -> int:
    """Cycles added when Direct-Page register low byte D.l != 0."""
    if op == 0xD4:                      # PEI
        return 1
    if op in _SPECIAL_BASE:
        return 0
    _, mode = _info(op)
    return 1 if mode in _DP_MODES else 0


def xcross_add(op: int) -> int:
    """Cycles added when an index crosses a 256-byte page (read ops only)."""
    if op in _SPECIAL_BASE:
        return 0
    mn, mode = _info(op)
    if mode not in _XCROSS_MODES:
        return 0
    if mn in _STORE_MNEMS:
        return 0                        # stores pay a fixed cost, no cross add
    return 1


def branch_class(op: int) -> int:
    """0 = not a branch, 1 = conditional (REL), 2 = BRA (always taken)."""
    if op == _BRA_OP:
        return 2
    if op in _CONDITIONAL_BRANCH_OPS:
        return 1
    return 0


def e_add(op: int) -> int:
    """Cycles added in native mode (e=0): RTI / BRK / COP."""
    return 1 if op in _E_PLUS1_OPS else 0


def instr_cpu_cycles(op: int, *, m: int = 1, x: int = 1, e: int = 0,
                     dp_low_nonzero: bool = False,
                     index_page_cross: bool = False,
                     branch_taken: bool = False,
                     branch_page_cross: bool = False,
                     move_bytes: Optional[int] = None) -> int:
    """Total CPU (bus) cycles for one execution of opcode `op`.

    Flags follow the 65816 convention: m/x are the width bits (1 = 8-bit,
    0 = 16-bit); e is the emulation bit (0 = native, the normal SNES game
    state). The dynamic predicates (D.l nonzero, page crossings, branch
    taken) are supplied by the caller; the recompiler resolves the
    statically-known ones at gen time and charges the rest at runtime.

    For MVN/MVP, pass `move_bytes` (= A+1 at execution); the routine returns
    7 * move_bytes. Without it, returns the per-byte cost (7).
    """
    if op in _BLOCK_MOVE_OPS:
        per_byte = _SPECIAL_BASE[op]    # 7
        return per_byte * move_bytes if move_bytes is not None else per_byte

    c = base_cpu_cycles(op)
    if m == 0:
        c += m_add(op)
    if x == 0:
        c += x_add(op)
    if dp_low_nonzero:
        c += dp_add(op)
    if index_page_cross:
        c += xcross_add(op)

    bc = branch_class(op)
    if bc == 1:                         # conditional
        if branch_taken:
            c += 1
            if e == 1 and branch_page_cross:
                c += 1
    elif bc == 2:                       # BRA (base already counts the take)
        if e == 1 and branch_page_cross:
            c += 1

    if e == 0:
        c += e_add(op)
    return c


# ==============================================================================
# Master-clock combiner (Layer 1 x Layer 2) -- documented first-cut, see header
# ==============================================================================

def instr_master_cycles(op: int, length: int, code_addr24: int,
                         data_addr24: Optional[int] = None, *,
                         memsel: int = 0, **cpu_kwargs) -> int:
    """First-cut master-clock cost for one instruction execution.

    Model (approximate; the bus-cycle-exact refinement is the cyc_watch
    milestone): the `length` opcode+operand fetch bytes are charged at the
    code region's speed; every remaining CPU cycle is charged at the data
    region's speed when a data address is given, else as an internal/fast
    cycle. Exactness is bounded here and nowhere else.
    """
    cyc = instr_cpu_cycles(op, **cpu_kwargs)
    code_speed = region_speed(code_addr24, memsel)
    rest_speed = region_speed(data_addr24, memsel) if data_addr24 is not None \
        else INTERNAL
    fetch = min(length, cyc)
    return fetch * code_speed + (cyc - fetch) * rest_speed


# ==============================================================================
# C header generation (keeps the runtime/reference engine drift-free)
# ==============================================================================

def _gen_c_header() -> str:
    base = [0] * 256
    madd = [0] * 256
    xadd = [0] * 256
    dpadd = [0] * 256
    xcross = [0] * 256
    bclass = [0] * 256
    eadd = [0] * 256
    valid = [0] * 256
    mode = [0xFF] * 256   # addressing mode (snes65816 constant); 0xFF = invalid
    for op in range(256):
        info = _info(op)
        if info is None:
            continue
        valid[op] = 1
        mode[op] = info[1]
        base[op] = base_cpu_cycles(op)
        madd[op] = m_add(op)
        xadd[op] = x_add(op)
        dpadd[op] = dp_add(op)
        xcross[op] = xcross_add(op)
        bclass[op] = branch_class(op)
        eadd[op] = e_add(op)

    # addressing-mode #defines, generated from the snes65816 constants so the
    # ring driver's mode comparisons can't drift from the decoder.
    _mode_names = ['IMP', 'ACC', 'IMM', 'DP', 'DP_X', 'DP_Y', 'ABS', 'ABS_X',
                   'ABS_Y', 'LONG', 'LONG_X', 'REL', 'REL16', 'STK', 'INDIR',
                   'INDIR_X', 'INDIR_Y', 'INDIR_LY', 'INDIR_L', 'INDIR_DPX',
                   'DP_INDIR', 'STK_IY']
    mode_defs = '\n'.join(
        f'#define SNES_MODE_{n:<9} {getattr(_d, n)}' for n in _mode_names)
    mode_defs += '\n#define SNES_MODE_INVALID 255'

    def arr(name, vals):
        rows = []
        for i in range(0, 256, 16):
            rows.append('    ' + ','.join(f'{v:2d}' for v in vals[i:i + 16]) + ',')
        return (f'static const uint8_t {name}[256] = {{\n'
                + '\n'.join(rows) + '\n};')

    block_move = ','.join(f'0x{op:02X}' for op in sorted(_BLOCK_MOVE_OPS))

    return f"""/* snes_cycles.h -- GENERATED by recompiler/snes_cycles.py. DO NOT EDIT.
 *
 * Authoritative 65816 / SNES cycle cost model, baked from the Python
 * authority so the runtime / reference engine never drifts from the
 * recompiler emitter. Regenerate with:
 *     python recompiler/snes_cycles.py --emit-c runner/src/snes/snes_cycles.h
 * Provenance and the full modifier rules live in snes_cycles.py.
 */
#ifndef SNES_CYCLES_H
#define SNES_CYCLES_H
#include <stdint.h>

/* Master-clock costs per memory access (21.47727 MHz master clock). */
#define SNES_CYC_FAST     6   /* 3.58 MHz */
#define SNES_CYC_SLOW     8   /* 2.68 MHz */
#define SNES_CYC_XSLOW   12   /* manual joypad / internal I/O */
#define SNES_CYC_INTERNAL SNES_CYC_FAST

/* Per-opcode static contributions (op = 1 byte). Combine via the inline
 * functions below; identical logic to snes_cycles.instr_cpu_cycles. */
{arr('SNES_BASE_CYCLES', base)}
{arr('SNES_M_ADD', madd)}       /* added when m==0 */
{arr('SNES_X_ADD', xadd)}       /* added when x==0 */
{arr('SNES_DP_ADD', dpadd)}     /* added when D.l != 0 */
{arr('SNES_XCROSS_ADD', xcross)} /* added when index crosses a page (read ops) */
{arr('SNES_BRANCH_CLASS', bclass)} /* 0 none, 1 conditional, 2 BRA */
{arr('SNES_E_ADD', eadd)}       /* added in native mode (e==0): RTI/BRK/COP */
{arr('SNES_OP_VALID', valid)}   /* 1 if opcode decodes */
{arr('SNES_OP_MODE', mode)}     /* addressing mode (SNES_MODE_*), 255 invalid */

/* Addressing-mode ids (mirror recompiler/snes65816.py constants). */
{mode_defs}

/* Master clocks for one access at a 24-bit address. memsel = $420D bit 0. */
static inline int snes_region_speed(uint32_t addr24, int memsel) {{
    uint8_t bank = (uint8_t)(addr24 >> 16);
    uint16_t addr = (uint16_t)(addr24 & 0xFFFF);
    if (bank >= 0x40 && bank <= 0x7F) return SNES_CYC_SLOW;
    if (bank >= 0xC0)                 return memsel ? SNES_CYC_FAST : SNES_CYC_SLOW;
    if (addr >= 0x8000) {{
        if (bank <= 0x3F)             return SNES_CYC_SLOW;       /* LoROM */
        return memsel ? SNES_CYC_FAST : SNES_CYC_SLOW;            /* WS2 LoROM */
    }}
    if (addr < 0x2000)                return SNES_CYC_SLOW;       /* WRAM mirror */
    if (addr < 0x4000)                return SNES_CYC_FAST;       /* PPU/APU/I/O */
    if (addr < 0x4200)                return SNES_CYC_XSLOW;      /* joypad/I/O */
    if (addr < 0x6000)                return SNES_CYC_FAST;       /* $4200-$5FFF */
    return SNES_CYC_SLOW;                                         /* $6000-$7FFF */
}}

/* MVN/MVP charge 7 master-equivalent CPU cycles PER BYTE moved; handle them
 * out of band (the byte count is the A register at execution). */
#define SNES_OP_IS_BLOCK_MOVE(op) ((op) == {block_move.split(',')[0]} || (op) == {block_move.split(',')[1]})

/* Total CPU (bus) cycles for one execution of `op`. Mirrors the Python
 * authority instr_cpu_cycles(); see snes_cycles.py for flag meanings. */
static inline int snes_instr_cpu_cycles(
        uint8_t op, int m, int x, int e,
        int dp_low_nonzero, int index_page_cross,
        int branch_taken, int branch_page_cross) {{
    int c = SNES_BASE_CYCLES[op];
    if (m == 0) c += SNES_M_ADD[op];
    if (x == 0) c += SNES_X_ADD[op];
    if (dp_low_nonzero) c += SNES_DP_ADD[op];
    if (index_page_cross) c += SNES_XCROSS_ADD[op];
    switch (SNES_BRANCH_CLASS[op]) {{
        case 1: /* conditional */
            if (branch_taken) {{ c += 1; if (e == 1 && branch_page_cross) c += 1; }}
            break;
        case 2: /* BRA: base already counts the take */
            if (e == 1 && branch_page_cross) c += 1;
            break;
        default: break;
    }}
    if (e == 0) c += SNES_E_ADD[op];
    return c;
}}

#endif /* SNES_CYCLES_H */
"""


def main(argv) -> int:
    import argparse
    ap = argparse.ArgumentParser(description='SNES cycle cost model / C emitter')
    ap.add_argument('--emit-c', metavar='PATH',
                    help='write the generated C header to PATH')
    args = ap.parse_args(argv)
    if args.emit_c:
        with open(args.emit_c, 'w', newline='\n') as f:
            f.write(_gen_c_header())
        print(f'wrote {args.emit_c}')
        return 0
    ap.print_help()
    return 0


if __name__ == '__main__':
    import sys
    sys.exit(main(sys.argv[1:]))
