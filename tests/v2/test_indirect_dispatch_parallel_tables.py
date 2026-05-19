"""Regression tests for indirect dispatches built from parallel tables."""
from _helpers import make_lorom_bank0  # noqa: E402

from v2.emit_function import emit_function  # noqa: E402


def test_parallel_byte_table_dispatch_uses_logical_index_register():
    """Module_MainRouting-style dispatches index parallel lo/hi/bank tables.

    The index register is the logical table index, not a byte offset into
    interleaved 2- or 3-byte entries. Codegen must not divide it by entry size.
    """
    targets = [0x9000 + i * 0x100 for i in range(8)]
    blobs = {
        0x8000: bytes([
            0xA5, 0x10,        # LDA $10
            0xA8,              # TAY
            0xB9, 0x00, 0x81,  # LDA $8100,Y ; low byte table
            0x85, 0x03,        # STA $03
            0xB9, 0x20, 0x81,  # LDA $8120,Y ; high byte table
            0x85, 0x04,        # STA $04
            0xB9, 0x40, 0x81,  # LDA $8140,Y ; bank byte table
            0x85, 0x05,        # STA $05
            0xDC, 0x03, 0x00,  # JML [$0003]
        ]),
        0x8100: bytes([t & 0xFF for t in targets]),
        0x8120: bytes([(t >> 8) & 0xFF for t in targets]),
        0x8140: bytes([0x00 for _ in targets]),
    }
    for target in targets:
        blobs[target] = bytes([0x6B])  # RTL

    rom = make_lorom_bank0(blobs)
    src = emit_function(
        rom=rom,
        bank=0,
        start=0x8000,
        entry_m=1,
        entry_x=1,
        func_name='Module_MainRouting_like',
        indirect_dispatch={
            0x008012: {
                'count': len(targets),
                'idx_reg': 'Y',
                'table_bases': (0x8100, 0x8120, 0x8140),
            },
        },
    )

    assert 'parallel byte tables: register already holds logical index' in src
    assert 'uint16 _idx = (uint16)(cpu->Y & 0xFFFF)' in src
    assert 'cpu->Y & 0xFFFF) / 3' not in src
    assert 'case 7:' in src
    assert 'bank_00_9700_M1X1(cpu)' in src
