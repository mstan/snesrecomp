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


def test_detects_accumulator_pop_push_return_address_adjustment():
    rom = make_lorom_bank0({
        0x8000: bytes([
            0xC2, 0x30,        # REP #$30
            0x68,              # PLA
            0xA8,              # TAY
            0x18,              # CLC
            0x69, 0x03, 0x00,  # ADC #$0003
            0x48,              # PHA
            0x60,              # RTS
        ]),
    })

    assert detect_inline_arg_bytes(rom, 0, 0x8000, entry_m=1, entry_x=1) == 3


def test_rejects_accumulator_after_unadjusted_restore():
    rom = make_lorom_bank0({
        0x8000: bytes([
            0xC2, 0x30,        # REP #$30
            0x68,              # PLA
            0x48,              # PHA
            0x18,              # CLC
            0x69, 0x03, 0x00,  # ADC #$0003
            0x48,              # PHA
            0x60,              # RTS
        ]),
    })

    assert detect_inline_arg_bytes(rom, 0, 0x8000, entry_m=1, entry_x=1) is None


def test_detects_x_pop_push_return_address_adjustment():
    rom = make_lorom_bank0({
        0x8000: bytes([
            0xE2, 0x20,        # SEP #$20
            0xC2, 0x10,        # REP #$10
            0xFA,              # PLX
            0x68,              # PLA
            0x48,              # PHA
            0xE8,              # INX
            0xBD, 0x00, 0x00,  # LDA $0000,X
            0xE8,              # INX
            0xDA,              # PHX
            0x6B,              # RTL
        ]),
    })

    assert detect_inline_arg_bytes(rom, 0, 0x8000, entry_m=1, entry_x=1) == 2


def test_x_carrier_is_invalidated_by_x_mutation():
    rom = make_lorom_bank0({
        0x8000: bytes([
            0xC2, 0x10,        # REP #$10
            0xFA,              # PLX
            0xE8,              # INX
            0xA2, 0x34, 0x12,  # LDX #$1234
            0xDA,              # PHX
            0x6B,              # RTL
        ]),
    })

    assert detect_inline_arg_bytes(rom, 0, 0x8000, entry_m=1, entry_x=1) is None


def test_x_carrier_is_invalidated_by_subroutine_call():
    rom = make_lorom_bank0({
        0x8000: bytes([
            0xC2, 0x10,              # REP #$10
            0xFA,                    # PLX
            0xE8,                    # INX
            0x22, 0x34, 0x12, 0x00,  # JSL $001234
            0xDA,                    # PHX
            0x6B,                    # RTL
        ]),
    })

    assert detect_inline_arg_bytes(rom, 0, 0x8000, entry_m=1, entry_x=1) is None


def test_rejects_x_save_restore_as_return_adjustment():
    rom = make_lorom_bank0({
        0x8000: bytes([
            0xC2, 0x10,        # REP #$10
            0xDA,              # PHX
            0x20, 0x00, 0x81,  # JSR $8100
            0xFA,              # PLX
            0xE8,              # INX
            0xDA,              # PHX
            0x20, 0x00, 0x81,  # JSR $8100
            0xFA,              # PLX
            0x60,              # RTS
        ]),
        0x8100: bytes([0x60]),
    })

    assert detect_inline_arg_bytes(rom, 0, 0x8000, entry_m=1, entry_x=1) is None


def test_rejects_a_save_restore_as_return_adjustment():
    rom = make_lorom_bank0({
        0x8000: bytes([
            0xC2, 0x20,        # REP #$20
            0x48,              # PHA
            0x20, 0x00, 0x81,  # JSR $8100
            0x68,              # PLA
            0x18,              # CLC
            0x69, 0x01, 0x00,  # ADC #$0001
            0x48,              # PHA
            0x20, 0x00, 0x81,  # JSR $8100
            0x68,              # PLA
            0x60,              # RTS
        ]),
        0x8100: bytes([0x60]),
    })

    assert detect_inline_arg_bytes(rom, 0, 0x8000, entry_m=1, entry_x=1) is None


def test_rejects_local_stack_slot_adjustment():
    rom = make_lorom_bank0({
        0x8000: bytes([
            0xC2, 0x30,        # REP #$30
            0xDA,              # PHX
            0xA3, 0x01,        # LDA $01,S
            0x18,              # CLC
            0x69, 0x01, 0x00,  # ADC #$0001
            0x83, 0x01,        # STA $01,S
            0xFA,              # PLX
            0x60,              # RTS
        ]),
    })

    assert detect_inline_arg_bytes(rom, 0, 0x8000, entry_m=1, entry_x=1) is None


def test_detects_return_slot_below_saved_register():
    rom = make_lorom_bank0({
        0x8000: bytes([
            0xC2, 0x30,        # REP #$30
            0xDA,              # PHX
            0xA3, 0x03,        # LDA $03,S
            0x18,              # CLC
            0x69, 0x03, 0x00,  # ADC #$0003
            0x83, 0x03,        # STA $03,S
            0xFA,              # PLX
            0x60,              # RTS
        ]),
    })

    assert detect_inline_arg_bytes(rom, 0, 0x8000, entry_m=1, entry_x=1) == 3


def test_rejects_return_slot_overwritten_by_push():
    rom = make_lorom_bank0({
        0x8000: bytes([
            0xC2, 0x30,        # REP #$30
            0xFA,              # PLX
            0x48,              # PHA
            0xA3, 0x01,        # LDA $01,S
            0x18,              # CLC
            0x69, 0x01, 0x00,  # ADC #$0001
            0x83, 0x01,        # STA $01,S
            0x68,              # PLA
            0xDA,              # PHX
            0x60,              # RTS
        ]),
    })

    assert detect_inline_arg_bytes(rom, 0, 0x8000, entry_m=1, entry_x=1) is None


def test_rejects_return_slot_overwritten_by_stack_store():
    rom = make_lorom_bank0({
        0x8000: bytes([
            0xC2, 0x30,        # REP #$30
            0xA9, 0x34, 0x12,  # LDA #$1234
            0x83, 0x01,        # STA $01,S
            0x68,              # PLA
            0x18,              # CLC
            0x69, 0x01, 0x00,  # ADC #$0001
            0x48,              # PHA
            0x60,              # RTS
        ]),
    })

    assert detect_inline_arg_bytes(rom, 0, 0x8000, entry_m=1, entry_x=1) is None


def test_rejects_partially_overwritten_return_slot():
    rom = make_lorom_bank0({
        0x8000: bytes([
            0xC2, 0x30,        # REP #$30
            0xE2, 0x20,        # SEP #$20
            0xA9, 0x12,        # LDA #$12
            0x83, 0x01,        # STA $01,S
            0xC2, 0x20,        # REP #$20
            0x68,              # PLA
            0x18,              # CLC
            0x69, 0x01, 0x00,  # ADC #$0001
            0x48,              # PHA
            0x60,              # RTS
        ]),
    })

    assert detect_inline_arg_bytes(rom, 0, 0x8000, entry_m=1, entry_x=1) is None


def test_rejects_adjustment_with_other_return_byte_overwritten():
    rom = make_lorom_bank0({
        0x8000: bytes([
            0xC2, 0x30,  # REP #$30
            0xE2, 0x20,  # SEP #$20
            0xA9, 0x12,  # LDA #$12
            0x83, 0x02,  # STA $02,S
            0x68,        # PLA
            0x18,        # CLC
            0x69, 0x01,  # ADC #$01
            0x48,        # PHA
            0x60,        # RTS
        ]),
    })

    assert detect_inline_arg_bytes(rom, 0, 0x8000, entry_m=1, entry_x=1) is None


def test_detects_return_adjustment_after_unmodified_restore():
    rom = make_lorom_bank0({
        0x8000: bytes([
            0xC2, 0x30,        # REP #$30
            0x68,              # PLA
            0x48,              # PHA
            0x68,              # PLA
            0x18,              # CLC
            0x69, 0x01, 0x00,  # ADC #$0001
            0x48,              # PHA
            0x60,              # RTS
        ]),
    })

    assert detect_inline_arg_bytes(rom, 0, 0x8000, entry_m=1, entry_x=1) == 1


def test_detects_return_adjustment_after_balanced_php_plp():
    rom = make_lorom_bank0({
        0x8000: bytes([
            0x08,        # PHP
            0x78,        # SEI
            0x28,        # PLP
            0xC2, 0x30,  # REP #$30
            0xFA,        # PLX
            0xE8,        # INX
            0xDA,        # PHX
            0x60,        # RTS
        ]),
    })

    assert detect_inline_arg_bytes(rom, 0, 0x8000, entry_m=1, entry_x=1) == 1


def test_rejects_unknown_plp_state():
    rom = make_lorom_bank0({
        0x8000: bytes([
            0x28,        # PLP
            0xC2, 0x30,  # REP #$30
            0xFA,        # PLX
            0xE8,        # INX
            0xDA,        # PHX
            0x60,        # RTS
        ]),
    })

    assert detect_inline_arg_bytes(rom, 0, 0x8000, entry_m=1, entry_x=1) is None


def test_rejects_plp_after_status_slot_overwrite():
    rom = make_lorom_bank0({
        0x8000: bytes([
            0xC2, 0x30,  # REP #$30
            0x08,        # PHP
            0xAB,        # PLB
            0x48,        # PHA
            0xAB,        # PLB
            0x28,        # PLP
            0xFA,        # PLX
            0xE8,        # INX
            0xDA,        # PHX
            0x60,        # RTS
        ]),
    })

    assert detect_inline_arg_bytes(rom, 0, 0x8000, entry_m=1, entry_x=1) is None


def test_rejects_xce():
    rom = make_lorom_bank0({
        0x8000: bytes([
            0xFB,        # XCE
            0xC2, 0x30,  # REP #$30
            0xFA,        # PLX
            0xE8,        # INX
            0xDA,        # PHX
            0x60,        # RTS
        ]),
    })

    assert detect_inline_arg_bytes(rom, 0, 0x8000, entry_m=1, entry_x=1) is None


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


def test_x_carrier_is_invalidated_by_block_move():
    for opcode in (0x44, 0x54):  # MVP / MVN
        rom = make_lorom_bank0({
            0x8000: bytes([
                0xC2, 0x10,       # REP #$10
                0xFA,             # PLX
                opcode, 0x00, 0x00,
                0xE8,             # INX
                0xDA,             # PHX
                0x60,             # RTS
            ]),
        })

        assert detect_inline_arg_bytes(
            rom, 0, 0x8000, entry_m=1, entry_x=1) is None


def test_y_carrier_is_invalidated_by_block_move():
    for opcode in (0x44, 0x54):  # MVP / MVN
        rom = make_lorom_bank0({
            0x8000: bytes([
                0xC2, 0x30,        # REP #$30
                0xA3, 0x01,        # LDA $01,S
                0xA8,              # TAY
                opcode, 0x00, 0x00,
                0x98,              # TYA
                0x18,              # CLC
                0x69, 0x01, 0x00,  # ADC #$0001
                0x83, 0x01,        # STA $01,S
                0x60,              # RTS
            ]),
        })

        assert detect_inline_arg_bytes(
            rom, 0, 0x8000, entry_m=1, entry_x=1) is None


def test_detects_sec_adc_return_adjustment():
    rom = make_lorom_bank0({
        0x8000: bytes([
            0xC2, 0x30,        # REP #$30
            0x68,              # PLA
            0x38,              # SEC
            0x69, 0x01, 0x00,  # ADC #$0001
            0x48,              # PHA
            0x60,              # RTS
        ]),
    })

    assert detect_inline_arg_bytes(rom, 0, 0x8000, entry_m=1, entry_x=1) == 2


def test_rejects_adc_with_unknown_carry():
    rom = make_lorom_bank0({
        0x8000: bytes([
            0xC2, 0x30,        # REP #$30
            0x68,              # PLA
            0x69, 0x01, 0x00,  # ADC #$0001
            0x48,              # PHA
            0x60,              # RTS
        ]),
    })

    assert detect_inline_arg_bytes(rom, 0, 0x8000, entry_m=1, entry_x=1) is None


def test_detects_adjustment_after_known_zero_delta_restore():
    rom = make_lorom_bank0({
        0x8000: bytes([
            0xC2, 0x30,        # REP #$30
            0x68,              # PLA
            0x18,              # CLC
            0x69, 0x00, 0x00,  # ADC #$0000
            0x48,              # PHA
            0x68,              # PLA
            0x18,              # CLC
            0x69, 0x01, 0x00,  # ADC #$0001
            0x48,              # PHA
            0x60,              # RTS
        ]),
    })

    assert detect_inline_arg_bytes(rom, 0, 0x8000, entry_m=1, entry_x=1) == 1


def test_8bit_adc_wrap_is_not_an_adjustment():
    rom = make_lorom_bank0({
        0x8000: bytes([
            0xE2, 0x20,  # SEP #$20
            0x68,        # PLA
            0x38,        # SEC
            0x69, 0xFF,  # ADC #$FF
            0x48,        # PHA
            0x60,        # RTS
        ]),
    })

    assert detect_inline_arg_bytes(rom, 0, 0x8000, entry_m=1, entry_x=1) is None


def test_rejects_adjustment_larger_than_result_type():
    rom = make_lorom_bank0({
        0x8000: bytes([
            0xC2, 0x30,        # REP #$30
            0x68,              # PLA
            0x18,              # CLC
            0x69, 0x00, 0x01,  # ADC #$0100
            0x48,              # PHA
            0x60,              # RTS
        ]),
    })

    assert detect_inline_arg_bytes(rom, 0, 0x8000, entry_m=1, entry_x=1) is None


def test_a_carrier_survives_local_push():
    rom = make_lorom_bank0({
        0x8000: bytes([
            0xC2, 0x30,        # REP #$30
            0xA3, 0x01,        # LDA $01,S
            0x18,              # CLC
            0x69, 0x03, 0x00,  # ADC #$0003
            0x48,              # PHA
            0xFA,              # PLX
            0x83, 0x01,        # STA $01,S
            0x60,              # RTS
        ]),
    })

    assert detect_inline_arg_bytes(rom, 0, 0x8000, entry_m=1, entry_x=1) == 3


def test_x_carrier_survives_local_push():
    rom = make_lorom_bank0({
        0x8000: bytes([
            0xC2, 0x30,  # REP #$30
            0xFA,        # PLX
            0xE8,        # INX
            0x48,        # PHA
            0xDA,        # PHX
            0x7A,        # PLY
            0x68,        # PLA
            0xDA,        # PHX
            0x60,        # RTS
        ]),
    })

    assert detect_inline_arg_bytes(rom, 0, 0x8000, entry_m=1, entry_x=1) == 1


def test_y_carrier_survives_local_push():
    rom = make_lorom_bank0({
        0x8000: bytes([
            0xC2, 0x30,        # REP #$30
            0x68,              # PLA
            0xA8,              # TAY
            0x48,              # PHA
            0x5A,              # PHY
            0xFA,              # PLX
            0x2B,              # PLD
            0x98,              # TYA
            0x18,              # CLC
            0x69, 0x03, 0x00,  # ADC #$0003
            0x48,              # PHA
            0x60,              # RTS
        ]),
    })

    assert detect_inline_arg_bytes(rom, 0, 0x8000, entry_m=1, entry_x=1) == 3


def test_rejects_adc_in_decimal_mode_after_sed():
    rom = make_lorom_bank0({
        0x8000: bytes([
            0xC2, 0x30,        # REP #$30
            0x68,              # PLA
            0xF8,              # SED
            0x18,              # CLC
            0x69, 0x09, 0x00,  # ADC #$0009 - BCD, not a byte count
            0x48,              # PHA
            0x60,              # RTS
        ]),
    })

    assert detect_inline_arg_bytes(rom, 0, 0x8000, entry_m=1, entry_x=1) is None


def test_rejects_adc_in_decimal_mode_after_sep_08():
    rom = make_lorom_bank0({
        0x8000: bytes([
            0xC2, 0x30,        # REP #$30
            0x68,              # PLA
            0xE2, 0x08,        # SEP #$08 - sets D
            0x18,              # CLC
            0x69, 0x09, 0x00,  # ADC #$0009
            0x48,              # PHA
            0x60,              # RTS
        ]),
    })

    assert detect_inline_arg_bytes(rom, 0, 0x8000, entry_m=1, entry_x=1) is None


def test_php_plp_restores_the_decimal_flag():
    rom = make_lorom_bank0({
        0x8000: bytes([
            0xC2, 0x30,        # REP #$30
            0x08,              # PHP - records binary mode
            0xF8,              # SED
            0x28,              # PLP - back to binary
            0x68,              # PLA
            0x18,              # CLC
            0x69, 0x03, 0x00,  # ADC #$0003
            0x48,              # PHA
            0x60,              # RTS
        ]),
    })

    assert detect_inline_arg_bytes(rom, 0, 0x8000, entry_m=1, entry_x=1) == 3


def test_accepts_adc_after_decimal_mode_is_cleared_again():
    rom = make_lorom_bank0({
        0x8000: bytes([
            0xC2, 0x30,        # REP #$30
            0x68,              # PLA
            0xF8,              # SED
            0xD8,              # CLD - back to binary before the add
            0x18,              # CLC
            0x69, 0x09, 0x00,  # ADC #$0009
            0x48,              # PHA
            0x60,              # RTS
        ]),
    })

    assert detect_inline_arg_bytes(rom, 0, 0x8000, entry_m=1, entry_x=1) == 9
