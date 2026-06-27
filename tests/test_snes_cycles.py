"""Datasheet pinning tests for the Axis-2 cycle cost model
(recompiler/snes_cycles.py).

Every assertion here is a number traceable to the provenance sources cited in
snes_cycles.py (undisbeliever 65816 opcode table + fullsnes/nesdev speed map).
This is the "reference shelf, not self-agreement" gate for Layer 1 (CPU
cycles) and Layer 2 (master-clock speed map): if a base or modifier silently
drifts, a documented value breaks here.

The model is keyed off the shared snes65816 decoder, so it also fails closed
if a future opcode-table edit changes a mnemonic/mode out from under it.
"""
import pathlib
import sys

_REPO = pathlib.Path(__file__).resolve().parent.parent
for _p in (str(_REPO / 'recompiler'), str(_REPO)):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import snes_cycles as sc  # noqa: E402


def _cyc(op, **kw):
    return sc.instr_cpu_cycles(op, **kw)


# --- Layer 1: base cycles (m=1, x=1, D.l=0, native, not taken) ---------------

def test_base_cycles_match_datasheet():
    # opcode : expected base CPU cycles (undisbeliever minimum case)
    cases = {
        0xA9: 2,   # LDA #imm
        0xAD: 4,   # LDA abs
        0xAF: 5,   # LDA long
        0xA5: 3,   # LDA dp
        0xB2: 5,   # LDA (dp)
        0xA7: 6,   # LDA [dp]
        0xBD: 4,   # LDA abs,X (read)
        0xBF: 5,   # LDA long,X
        0xB5: 4,   # LDA dp,X
        0xA1: 6,   # LDA (dp,X)
        0xB1: 5,   # LDA (dp),Y (read)
        0xB7: 6,   # LDA [dp],Y
        0xA3: 4,   # LDA sr
        0xB3: 7,   # LDA (sr,S),Y
        0x9D: 5,   # STA abs,X (store base differs from the read 4)
        0x99: 5,   # STA abs,Y (store)
        0x91: 6,   # STA (dp),Y (store)
        0x0E: 6,   # ASL abs (RMW)
        0x06: 5,   # ASL dp (RMW)
        0x1E: 7,   # ASL abs,X (RMW)
        0x16: 6,   # ASL dp,X (RMW)
        0x0A: 2,   # ASL A (accumulator)
        0x1A: 2,   # INC A
        0xEE: 6,   # INC abs
        0xAA: 2,   # TAX
        0x18: 2,   # CLC
        0x20: 6,   # JSR abs
        0xFC: 8,   # JSR (abs,X)
        0x22: 8,   # JSL long
        0x4C: 3,   # JMP abs
        0x5C: 4,   # JML long
        0x6C: 5,   # JMP (abs)
        0xDC: 6,   # JML [abs]
        0x7C: 6,   # JMP (abs,X)
        0x60: 6,   # RTS
        0x6B: 6,   # RTL
        0x48: 3,   # PHA
        0x68: 4,   # PLA
        0x08: 3,   # PHP
        0x0B: 4,   # PHD
        0x2B: 5,   # PLD
        0xF4: 5,   # PEA
        0xD4: 6,   # PEI
        0x62: 6,   # PER
        0x82: 4,   # BRL
        0xEB: 3,   # XBA
        0xFB: 2,   # XCE
        0xC2: 3,   # REP
        0xE2: 3,   # SEP
        0xEA: 2,   # NOP
    }
    for op, want in cases.items():
        got = _cyc(op)
        assert got == want, f'op ${op:02X} base: got {got}, want {want}'


# --- Layer 1: width / DP / page-cross / branch / native modifiers ------------

def test_m_width_modifier():
    assert _cyc(0xA9, m=0) == 3        # LDA #imm 16-bit
    assert _cyc(0xAD, m=0) == 5        # LDA abs 16-bit
    assert _cyc(0x0E, m=0) == 8        # ASL abs RMW: +2
    assert _cyc(0x06, m=0) == 7        # ASL dp RMW: +2
    assert _cyc(0x48, m=0) == 4        # PHA: +1
    assert _cyc(0x68, m=0) == 5        # PLA: +1
    assert _cyc(0xAA, m=0) == 2        # TAX unaffected by m


def test_x_width_modifier():
    assert _cyc(0xA2, x=0) == 3        # LDX #imm 16-bit
    assert _cyc(0xAE, x=0) == 5        # LDX abs
    assert _cyc(0xDA, x=0) == 4        # PHX: +1
    assert _cyc(0xFA, x=0) == 5        # PLX: +1
    assert _cyc(0xA9, x=0) == 2        # LDA unaffected by x


def test_dp_low_nonzero_modifier():
    assert _cyc(0xA5, dp_low_nonzero=True) == 4   # LDA dp +1
    assert _cyc(0xB5, dp_low_nonzero=True) == 5   # LDA dp,X +1
    assert _cyc(0xB1, dp_low_nonzero=True) == 6   # LDA (dp),Y +1
    assert _cyc(0xD4, dp_low_nonzero=True) == 7   # PEI +1
    assert _cyc(0xA3, dp_low_nonzero=True) == 4   # LDA sr: NOT a DP mode
    assert _cyc(0xAD, dp_low_nonzero=True) == 4   # LDA abs: unaffected


def test_index_page_cross_modifier():
    assert _cyc(0xBD, index_page_cross=True) == 5   # LDA abs,X read +1
    assert _cyc(0xB9, index_page_cross=True) == 5   # LDA abs,Y read +1
    assert _cyc(0xB1, index_page_cross=True) == 6   # LDA (dp),Y read +1
    assert _cyc(0x9D, index_page_cross=True) == 5   # STA abs,X store: no cross
    assert _cyc(0x91, index_page_cross=True) == 6   # STA (dp),Y store: no cross
    assert _cyc(0xBF, index_page_cross=True) == 5   # LDA long,X: never crosses


def test_branch_modifiers():
    assert _cyc(0xF0) == 2                                   # BEQ not taken
    assert _cyc(0xF0, branch_taken=True) == 3               # taken (native)
    assert _cyc(0xF0, branch_taken=True, e=1) == 3          # taken, no cross
    assert _cyc(0xF0, branch_taken=True, e=1,
                branch_page_cross=True) == 4                # taken+cross, emu
    assert _cyc(0xF0, branch_taken=True, e=0,
                branch_page_cross=True) == 3                # native: no cross +
    assert _cyc(0x80) == 3                                   # BRA always taken
    assert _cyc(0x80, e=1, branch_page_cross=True) == 4     # BRA emu cross


def test_native_emulation_modifier():
    assert _cyc(0x40, e=0) == 7        # RTI native +1
    assert _cyc(0x40, e=1) == 6        # RTI emulation
    assert _cyc(0x00, e=0) == 8        # BRK native +1
    assert _cyc(0x00, e=1) == 7        # BRK emulation
    assert _cyc(0x02, e=0) == 8        # COP native +1


def test_block_move():
    assert _cyc(0x54) == 7                       # MVN per byte
    assert _cyc(0x54, move_bytes=10) == 70       # MVN 10 bytes
    assert _cyc(0x44, move_bytes=1) == 7         # MVP 1 byte


# --- Layer 2: SNES master-clock speed map ------------------------------------

def test_region_speed_map():
    F, S, X = sc.FAST, sc.SLOW, sc.XSLOW
    cases = [
        # (addr24, memsel, expected)
        (0x000000, 0, S),   # $00:0000 WRAM mirror
        (0x001FFF, 0, S),
        (0x002100, 0, F),   # PPU regs
        (0x002140, 0, F),   # APU ports
        (0x003FFF, 0, F),
        (0x004000, 0, X),   # manual joypad XSlow
        (0x0041FF, 0, X),
        (0x004200, 0, F),   # $4200-$5FFF fast
        (0x005FFF, 0, F),
        (0x006000, 0, S),   # expansion slow
        (0x007FFF, 0, S),
        (0x008000, 0, S),   # $00:8000 LoROM slow
        (0x00FFFF, 0, S),
        (0x400000, 0, S),   # HiROM image slow
        (0x7E0000, 0, S),   # WRAM slow
        (0x7FFFFF, 0, S),
        (0x800000, 0, S),   # $80:0000 mirror of $00 WRAM
        (0x802100, 0, F),   # $80 mirror PPU regs fast
        (0x808000, 0, S),   # $80:8000 WS2 LoROM, slow without FastROM
        (0x808000, 1, F),   # ...fast with FastROM
        (0x80FFFF, 1, F),
        (0xC00000, 0, S),   # $C0 WS2 HiROM slow without FastROM
        (0xC00000, 1, F),   # ...fast with FastROM
        (0xFFFFFF, 1, F),
    ]
    for addr, memsel, want in cases:
        got = sc.region_speed(addr, memsel)
        assert got == want, \
            f'region ${addr:06X} memsel={memsel}: got {got}, want {want}'


# --- Coverage + generated-header consistency ---------------------------------

def test_every_decoded_opcode_has_a_base():
    import snes65816 as d
    for op in d.opcode_table():
        c = sc.base_cpu_cycles(op)
        assert c >= 2, f'op ${op:02X} base {c} < 2 (no instruction is < 2)'


def test_generated_header_matches_authority():
    """The checked-in snes_cycles.h must be regenerated from the authority;
    if this fails, run: python recompiler/snes_cycles.py --emit-c
    runner/src/snes/snes_cycles.h"""
    hdr = _REPO / 'runner' / 'src' / 'snes' / 'snes_cycles.h'
    if not hdr.exists():
        return  # header not generated yet in this checkout; skip
    on_disk = hdr.read_text(encoding='utf-8').replace('\r\n', '\n')
    fresh = sc._gen_c_header()
    assert on_disk == fresh, \
        'snes_cycles.h is stale vs snes_cycles.py; regenerate it (--emit-c)'
