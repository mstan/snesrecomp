"""Axis-2 step C: the v2 emitter charges each block's static 65816 CPU
cycles as a per-block constant (recompiler/snes_cycles.py via
emit_function._block_cycle_const). Guards the cost-model -> emitter wiring."""
import re

from _helpers import make_lorom_bank0  # noqa: E402
from v2.emit_function import emit_function  # noqa: E402


# A standalone per-block static charge: `    cpu->cycles += N;` on its own
# line (the dynamic charges are `if (...) cpu->cycles += 1; /* ... */`).
_STATIC_CHARGE = re.compile(r'^\s*cpu->cycles \+= (\d+);\s*$', re.M)


def test_linear_block_charges_static_cycles():
    # LDA #$05 (2) ; STA $00 (dp, 3) ; RTS (6) -> one block, 11 static cycles
    # (plus a runtime D.l!=0 dynamic charge for the dp store).
    rom = make_lorom_bank0({0x8000: bytes([0xA9, 0x05, 0x85, 0x00, 0x60])})
    src = emit_function(rom, bank=0, start=0x8000, entry_m=1, entry_x=1)
    charges = _STATIC_CHARGE.findall(src)
    assert charges == ['11'], f'expected one static block charge of 11, got {charges}'
    assert "if (cpu->D & 0xFF) cpu->cycles += 1;" in src  # dp dynamic present


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


def test_dp_dynamic_charge_emitted():
    # LDA $00 (DP mode) ; RTS — the D.l!=0 charge is runtime-conditional.
    rom = make_lorom_bank0({0x8000: bytes([0xA5, 0x00, 0x60])})
    src = emit_function(rom, bank=0, start=0x8000, entry_m=1, entry_x=1)
    assert "if (cpu->D & 0xFF) cpu->cycles += 1;" in src, src


def test_abs_x_page_cross_dynamic_charge_emitted():
    # LDA $1234,X (read) ; RTS — page-cross charge uses the static base $1234.
    rom = make_lorom_bank0({0x8000: bytes([0xBD, 0x34, 0x12, 0x60])})
    src = emit_function(rom, bank=0, start=0x8000, entry_m=1, entry_x=1)
    assert "0x1234 & 0xFF00" in src and "+ cpu->X) & 0xFF00)" in src, src
    assert "/* abs,X read page-cross */" in src


def test_taken_branch_charges_one_cycle():
    # BNE fork — the taken edge must add +1 cycle (block const = not-taken base).
    rom = make_lorom_bank0({0x8000: bytes([
        0xD0, 0x02,  # BNE $8004
        0xEA, 0x60,  # NOP; RTS
        0xEA, 0x60,  # NOP; RTS (taken)
    ])})
    src = emit_function(rom, bank=0, start=0x8000, entry_m=1, entry_x=1)
    assert re.search(r'if \(.*\) \{ cpu->cycles \+= 1; goto ', src), src


def test_store_abs_x_has_no_page_cross_charge():
    # STA $1234,X (store) — stores pay a fixed cost (in the base), no cross add.
    rom = make_lorom_bank0({0x8000: bytes([0x9D, 0x34, 0x12, 0x60])})
    src = emit_function(rom, bank=0, start=0x8000, entry_m=1, entry_x=1)
    assert "page-cross" not in src, src


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
