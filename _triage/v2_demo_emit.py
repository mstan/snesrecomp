import sys, pathlib
REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / 'recompiler'))
sys.path.insert(0, str(REPO / 'tests/v2'))
from _helpers import make_lorom_bank0
from v2.emit_function import emit_function

print("=" * 60)
print("Sample 1: linear function (LDA #$05; STA $00; RTS)")
print("=" * 60)
rom = make_lorom_bank0({0x8000: bytes([0xA9, 0x05, 0x85, 0x00, 0x60])})
print(emit_function(rom, bank=0, start=0x8000, entry_m=1, entry_x=1))

print("=" * 60)
print("Sample 2: cond branch + mode split")
print("=" * 60)
rom = make_lorom_bank0({
    0x8000: bytes([0xB0, 0x0A, 0xC2, 0x30, 0x80, 0x06]),  # BCS $800C, REP #$30, BRA $800C
    0x800C: bytes([0xEA, 0x60]),                          # NOP, RTS
})
print(emit_function(rom, bank=0, start=0x8000, entry_m=1, entry_x=1))
