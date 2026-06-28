"""Regression tests for same-function computed-goto runway dispatches."""

from _helpers import make_lorom_bank0  # noqa: E402

from v2.decoder import DecodeKey, addr24, decode_function  # noqa: E402
from v2.emit_function import emit_function  # noqa: E402


def _runway_rom():
    return make_lorom_bank0({
        0x8000: bytes([
            0x4A,              # LSR A
            0x85, 0x12,        # STA $12
            0x4A,              # LSR A
            0x65, 0x12,        # ADC $12
            0x18,              # CLC
            0x69, 0x20, 0x80,  # ADC #$8020
            0x85, 0x12,        # STA $12
            0xA9, 0xF0, 0x00,  # LDA #$00F0
            0xE2, 0x30,        # SEP #$30
            0x6C, 0x12, 0x00,  # JMP ($0012)
        ]),
        0x8020: bytes([
            0x8D, 0x70, 0x03,  # STA $0370
            0x8D, 0x74, 0x03,  # STA $0374
            0x8D, 0x78, 0x03,  # STA $0378
            0x8D, 0x7C, 0x03,  # STA $037C
            0x60,              # RTS
        ]),
    })


def test_decoder_recovers_local_stride_runway_dispatch():
    rom = _runway_rom()
    graph = decode_function(
        rom, bank=0, start=0x8000, entry_m=0, entry_x=0, end=0x802D)

    key = DecodeKey(addr24(0, 0x8011), 1, 1, ())
    assert key in graph.insns
    insn = graph.insns[key].insn
    assert insn.dispatch_local_goto is True
    assert [e & 0xFFFF for e in insn.dispatch_entries] == [
        0x8020, 0x8023, 0x8026, 0x8029,
    ]
    assert not graph.unresolved_indirects

    for pc in (0x8020, 0x8023, 0x8026, 0x8029):
        assert DecodeKey(addr24(0, pc), 1, 1, ()) in graph.insns


def test_codegen_emits_local_gotos_not_handler_calls():
    src = emit_function(
        rom=_runway_rom(),
        bank=0,
        start=0x8000,
        entry_m=0,
        entry_x=0,
        end=0x802D,
        func_name='LocalStrideRunway',
    )

    assert 'local computed-goto dispatch' in src
    assert 'uint16 _target = cpu_read16(cpu, cpu->PB, (uint16)0x0012)' in src
    assert 'case 0x8020: goto L_8020_M1X1;' in src
    assert 'case 0x8029: goto L_8029_M1X1;' in src
    assert 'bank_00_8020_M1X1(cpu)' not in src
    assert 'bank_00_8029_M1X1(cpu)' not in src
