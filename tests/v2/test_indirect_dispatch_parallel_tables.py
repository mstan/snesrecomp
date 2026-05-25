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


def test_plain_indirect_dispatch_switches_on_runtime_mx():
    """Plain JMP (abs,X) dispatches to the runtime M/X variant.

    Unlike ExecutePtr-style trampolines, a direct indirect jump does not
    force SEP #$30 before entering the selected handler. The generated
    dispatcher therefore emits all variants and selects by cpu flags.
    """
    rom = make_lorom_bank0({
        0x8000: bytes([
            0xC2, 0x30,        # REP #$30  ; M0X0
            0xA2, 0x00, 0x00,  # LDX #$0000
            0x7C, 0x00, 0x81,  # JMP ($8100,X)
        ]),
        0x8100: bytes([
            0x00, 0x90,        # -> $9000
        ]),
        0x9000: bytes([0x60]),  # RTS
    })

    src = emit_function(
        rom=rom,
        bank=0,
        start=0x8000,
        entry_m=1,
        entry_x=1,
        func_name='PlainIndirectDispatchSiteMx',
        indirect_dispatch={
            0x008005: {
                'count': 1,
                'idx_reg': 'X',
            },
        },
    )

    assert 'indirect dispatch terminator: cfg-resolved target list' in src
    assert 'switch (((cpu->m_flag & 1) << 1) | (cpu->x_flag & 1))' in src
    for sfx in ("_M0X0", "_M0X1", "_M1X0", "_M1X1"):
        assert f'bank_00_9000{sfx}(cpu)' in src


def test_absolute_indirect_dispatch_switches_on_loaded_pointer():
    """JMP ($dp) dispatches by the pointer, even if X is reused.

    Zelda room-object drawing loads the handler pointer into $0e, then
    overwrites X with an object-data offset before `JMP ($000e)`. The
    switch key must be the runtime pointer, not X.
    """
    rom = make_lorom_bank0({
        0x8000: bytes([
            0xC2, 0x30,        # REP #$30  ; 16-bit A/X/Y
            0xA2, 0x00, 0x00,  # LDX #$0000
            0xBD, 0x00, 0x82,  # LDA $8200,X ; handler pointer
            0x85, 0x0E,        # STA $0e
            0xBD, 0x00, 0x81,  # LDA $8100,X ; object data offset
            0xAA,              # TAX
            0x6C, 0x0E, 0x00,  # JMP ($000e)
        ]),
        0x8100: bytes([
            0xD8, 0x03,
            0xE8, 0x02,
        ]),
        0x8200: bytes([
            0x00, 0x90,
            0x00, 0x91,
        ]),
        0x9000: bytes([0x60]),  # RTS
        0x9100: bytes([0x60]),  # RTS
    })

    src = emit_function(
        rom=rom,
        bank=0,
        start=0x8000,
        entry_m=1,
        entry_x=1,
        func_name='AbsoluteIndirectPointerDispatch',
        indirect_dispatch={
            0x00800E: {
                'count': 2,
                'idx_reg': 'X',
                'table_bases': (0x8200,),
            },
        },
    )

    assert 'absolute indirect dispatch: switch on the loaded pointer' in src
    assert 'uint16 _target = cpu_read16(cpu, cpu->PB, (uint16)0x000e)' in src
    assert 'switch (_target)' in src
    assert 'case 0x9000:' in src
    assert 'case 0x9100:' in src
    assert 'cpu->X & 0xFFFF) / 2' not in src
