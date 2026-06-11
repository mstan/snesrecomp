"""Regression tests for JSR/JSL inline-argument routine detection."""

from _helpers import make_lorom_bank0  # noqa: E402

from v2.decoder import detect_inline_arg_bytes  # noqa: E402


def test_detects_direct_a_stack_return_address_adjustment():
    rom = make_lorom_bank0({
        0x8000: bytes([
            0xC2, 0x30,        # REP #$30
            0xA3, 0x01,        # LDA $01,S
            0x18,              # CLC
            0x69, 0x03, 0x00,  # ADC #$0003
            0x83, 0x01,        # STA $01,S
            0x6B,              # RTL
        ]),
    })

    assert detect_inline_arg_bytes(rom, 0, 0x8000, entry_m=1, entry_x=1) == 3


def test_detects_y_carried_stack_return_address_adjustment():
    rom = make_lorom_bank0({
        0x8000: bytes([
            0x08,              # PHP
            0x8B,              # PHB
            0xC2, 0x30,        # REP #$30
            0xA3, 0x04,        # LDA $04,S
            0x48,              # PHA
            0xAB,              # PLB
            0xAB,              # PLB
            0xA3, 0x03,        # LDA $03,S
            0xA8,              # TAY
            0xB9, 0x01, 0x00,  # LDA $0001,Y
            0x29, 0xFF, 0x00,  # AND #$00FF
            0xAA,              # TAX
            0x98,              # TYA
            0x18,              # CLC
            0x69, 0x08, 0x00,  # ADC #$0008
            0x83, 0x03,        # STA $03,S
            0xAB,              # PLB
            0x28,              # PLP
            0x6B,              # RTL
        ]),
    })

    assert detect_inline_arg_bytes(rom, 0, 0x8000, entry_m=1, entry_x=1) == 8


def test_y_carrier_is_invalidated_by_y_mutation():
    rom = make_lorom_bank0({
        0x8000: bytes([
            0xC2, 0x30,        # REP #$30
            0xA3, 0x03,        # LDA $03,S
            0xA8,              # TAY
            0xC8,              # INY
            0x98,              # TYA
            0x18,              # CLC
            0x69, 0x08, 0x00,  # ADC #$0008
            0x83, 0x03,        # STA $03,S
            0x6B,              # RTL
        ]),
    })

    assert detect_inline_arg_bytes(rom, 0, 0x8000, entry_m=1, entry_x=1) is None
