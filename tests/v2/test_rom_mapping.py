from snes65816 import (
    ROM_MAP_HIROM,
    ROM_MAP_LOROM,
    detect_rom_mapping,
    get_rom_mapping,
    is_rom_address,
    rom_offset,
    set_rom_mapping,
    vector_table_offset,
)


def _hirom_fixture():
    rom = bytearray([0xFF] * 0x10000)
    header = 0xFFC0
    rom[header + 0x15] = 0x31
    rom[header + 0x1C:header + 0x20] = bytes(
        [0xCB, 0xED, 0x34, 0x12])
    rom[header + 0x3C:header + 0x3E] = bytes([0xF7, 0x83])
    return bytes(rom)


def test_hirom_header_selects_vector_table_and_full_bank_mapping():
    previous = get_rom_mapping()
    try:
        rom = _hirom_fixture()
        assert detect_rom_mapping(rom) == ROM_MAP_HIROM
        assert vector_table_offset(rom) == 0xFFE0

        set_rom_mapping(ROM_MAP_HIROM)
        assert rom_offset(0x00, 0x83F7) == 0x83F7
        assert rom_offset(0x80, 0x83F7) == 0x83F7
        assert rom_offset(0xC0, 0x83F7) == 0x83F7
        assert rom_offset(0xC1, 0x1234) == 0x11234
        assert is_rom_address(0xC1, 0x1234)
        assert is_rom_address(0xFE, 0x1234)
        assert not is_rom_address(0x01, 0x1234)
        assert not is_rom_address(0x7E, 0x8000)
        assert not is_rom_address(0x7F, 0xFFFF)
    finally:
        set_rom_mapping(previous)


def test_ambiguous_header_preserves_lorom_compatibility_default():
    rom = bytes([0xFF] * 0x10000)
    assert detect_rom_mapping(rom) == ROM_MAP_LOROM
    assert vector_table_offset(rom) == 0x7FE0
