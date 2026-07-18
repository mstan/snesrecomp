"""Explicit runtime-pointer target universes from decomp structure."""
from _helpers import make_lorom_bank0  # noqa: E402

from v2.emit_function import emit_function  # noqa: E402


def test_indexed_ptrtail_switches_on_loaded_pointer_not_index():
    rom = make_lorom_bank0({
        0x8000: bytes([0x7C, 0x10, 0x00]),  # JMP ($0010,X)
        0x9000: bytes([0x60]),
        0x9100: bytes([0x60]),
    })
    src = emit_function(
        rom=rom, bank=0, start=0x8000, entry_m=0, entry_x=0,
        func_name="IndexedPointerTail",
        indirect_dispatch={
            0x008000: {
                'count': 2, 'idx_reg': 'X', 'table_bases': (),
                'ptr_call': False, 'pointer_match': True,
                'targets': (0x9000, 0x9100),
            },
        },
    )
    assert "uint16 _ptr = (uint16)(0x0010 + (cpu->X & 0xFFFF))" in src
    assert "uint16 _target = cpu_read16(cpu, cpu->PB, _ptr)" in src
    assert "switch (_target)" in src
    assert "case 0x9000:" in src
    assert "case 0x9100:" in src
    assert "cpu->X & 0xFFFF) / 2" not in src
    assert "uint8 _saved_pb" not in src
    assert "cpu->PB = 0x00" not in src
    assert "case 0: cpu_tailcall_inherit_return_context(_entry_s, _hrv);" in src
    assert "return _r;" in src


def test_long_ptrtail_fallback_uses_loaded_bank_without_pb_overlay():
    rom = make_lorom_bank0({
        0x8000: bytes([0xDC, 0x10, 0x00]),  # JML [$0010]
    })
    src = emit_function(
        rom=rom, bank=0, start=0x8000, entry_m=0, entry_x=0,
        func_name="LongPointerTail",
        indirect_dispatch={
            0x008000: {
                'count': 2, 'idx_reg': 'X', 'table_bases': (),
                'ptr_call': False, 'pointer_match': True,
                'targets': (0x019000, 0x029000),
            },
        },
    )
    assert "uint32 _target = ((uint32)_target_bank << 16)" in src
    assert "interp_tier_dispatch_tail(cpu, _target," in src
    assert "((uint32)cpu->PB << 16) | _target" not in src
    assert "cpu->PB = 0x01;  /* long indirect tail transfer */" in src
    assert "cpu->PB = 0x02;  /* long indirect tail transfer */" in src
    assert "uint8 _saved_pb" not in src


def test_popped_call_ptrtail_inherits_context_installed_by_terminal_caller():
    rom = make_lorom_bank0({
        0x8000: bytes([0x7C, 0x10, 0x00]),
        0x9000: bytes([0x60]),
    })
    src = emit_function(
        rom=rom, bank=0, start=0x8000, entry_m=0, entry_x=0,
        func_name="PoppedCallPointerTail",
        indirect_dispatch={
            0x008000: {
                'count': 1, 'idx_reg': 'X', 'table_bases': (),
                'ptr_call': False, 'pointer_match': True,
                'popped_call_frame': True,
                'targets': (0x9000,),
            },
        },
    )
    assert "CPU_HOST_RETURN_CONSUMED_CALL" not in src
    assert "cpu_tailcall_inherit_return_context(_entry_s, _hrv);" in src
    assert (
        "cpu_tailcall_inherit_return_context("
        "(uint16)(_entry_s + 2u), 3);" not in src)
    assert (
        "interp_tier_dispatch_tail(cpu, 0x009000u, 0x008000u, "
        "_entry_s, _hrv)" in src)
