"""Shared helpers for v2 decoder/cfg/ir tests."""
import pathlib
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent.parent
if str(REPO / 'recompiler') not in sys.path:
    sys.path.insert(0, str(REPO / 'recompiler'))


def make_lorom_bank0(blobs: dict) -> bytes:
    """Build a 32KB LoROM bank-0 image. `blobs` maps local PC ($8000+)
    to the bytes that should appear there. Padding is zero."""
    rom = bytearray(0x8000)
    for pc, blob in blobs.items():
        assert 0x8000 <= pc <= 0xFFFF, f"PC ${pc:04X} out of LoROM bank-0 range"
        off = pc - 0x8000
        end = off + len(blob)
        assert end <= 0x8000, f"blob at ${pc:04X} overruns bank end"
        rom[off:end] = blob
    return bytes(rom)
