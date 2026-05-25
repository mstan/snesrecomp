"""Decoder coverage for callees with multiple possible exit M/X states."""
from _helpers import make_lorom_bank0  # noqa: E402

from v2.decoder import DecodeKey, decode_function  # noqa: E402


def test_ambiguous_call_exit_modes_decode_all_return_site_modes():
    # $8000: JSL $00:8100
    # $8004: BCS $800A
    # $8006: LDA #imm  ; width depends on the callee's runtime M exit
    # $8009: RTS
    # $800A: RTS
    rom = make_lorom_bank0({
        0x8000: bytes([
            0x22, 0x00, 0x81, 0x00,
            0xB0, 0x04,
            0xA9, 0x12, 0xEA,
            0x60,
            0x60,
        ]),
    })
    modes = {
        (0x008100, 0, 0): frozenset({(0, 0), (1, 0)}),
    }

    graph = decode_function(
        rom, bank=0, start=0x8000, entry_m=0, entry_x=0,
        callee_exit_mx_modes=modes)

    keys_at_return = {
        (k.m, k.x) for k in graph.insns
        if (k.pc & 0xFFFF) == 0x8004
    }
    assert keys_at_return == {(0, 0), (1, 0)}

    keys_at_lda = {
        (k.m, k.x) for k in graph.insns
        if (k.pc & 0xFFFF) == 0x8006
    }
    assert keys_at_lda == {(0, 0), (1, 0)}


def test_without_ambiguous_modes_call_preserves_static_mode():
    rom = make_lorom_bank0({
        0x8000: bytes([0x22, 0x00, 0x81, 0x00, 0x60]),
    })

    graph = decode_function(rom, bank=0, start=0x8000, entry_m=0, entry_x=0)

    assert DecodeKey(0x008004, 0, 0) in graph.insns
    assert DecodeKey(0x008004, 1, 0) not in graph.insns
