"""Axis-2 step C: the v2 emitter charges each block's static 65816 CPU
cycles as a per-block constant (recompiler/snes_cycles.py via
emit_function._block_cycle_const). Guards the cost-model -> emitter wiring."""
import re

from _helpers import make_lorom_bank0  # noqa: E402
from v2.emit_function import emit_function  # noqa: E402


def test_linear_block_charges_static_cycles():
    # LDA #$05 (2) ; STA $00 (dp, 3) ; RTS (6) -> one block, 11 CPU cycles.
    rom = make_lorom_bank0({0x8000: bytes([0xA9, 0x05, 0x85, 0x00, 0x60])})
    src = emit_function(rom, bank=0, start=0x8000, entry_m=1, entry_x=1)
    charges = re.findall(r'cpu->cycles \+= (\d+);', src)
    assert charges == ['11'], f'expected one block charge of 11, got {charges}'


def test_width_widens_static_charge():
    # 16-bit (REP #$30) LDA #$1234 (3) ; RTS (6) -> 9. The native REP itself
    # (CLC/XCE/REP) live in the entry block; assert the 16-bit LDA path adds
    # the m=0 cycle (base 2 + 1). We check the total contains a charge whose
    # value reflects 16-bit accounting (>= the 8-bit equivalent).
    rom8 = make_lorom_bank0({0x8000: bytes([0xA9, 0x05, 0x60])})            # LDA# 8b ; RTS
    rom16 = make_lorom_bank0({0x8000: bytes([0xC2, 0x20, 0xA9, 0x34, 0x12, 0x60])})  # REP#$20; LDA#16b; RTS
    s8 = emit_function(rom8, bank=0, start=0x8000, entry_m=1, entry_x=1)
    s16 = emit_function(rom16, bank=0, start=0x8000, entry_m=1, entry_x=1)
    c8 = sum(int(x) for x in re.findall(r'cpu->cycles \+= (\d+);', s8))
    c16 = sum(int(x) for x in re.findall(r'cpu->cycles \+= (\d+);', s16))
    # 8b: LDA# 2 + RTS 6 = 8. 16b: REP 3 + LDA#(2+1) + RTS 6 = 12.
    assert c8 == 8, f'8-bit total {c8} != 8'
    assert c16 == 12, f'16-bit total {c16} != 12 (m=0 LDA should add 1)'


def test_every_block_with_insns_is_charged():
    # BCS fork -> three blocks (entry, fall-through, taken), each non-empty,
    # so each must carry a cpu->cycles charge.
    rom = make_lorom_bank0({0x8000: bytes([
        0xB0, 0x02,  # BCS $8004
        0xEA, 0x60,  # NOP; RTS (fall-through)
        0xEA, 0x60,  # NOP; RTS (taken)
    ])})
    src = emit_function(rom, bank=0, start=0x8000, entry_m=1, entry_x=1)
    charges = re.findall(r'cpu->cycles \+= (\d+);', src)
    assert len(charges) >= 3, f'expected a charge per block (>=3), got {charges}'
