"""Regression tests for PHA/SEP/RTS table dispatchers."""
from _helpers import make_lorom_bank0  # noqa: E402

from v2 import codegen
from v2.emit_function import emit_function  # noqa: E402


def test_pha_rts_stack_dispatch_replaces_fake_return_push():
    """PHA of target-1 followed by SEP #$30 + RTS is a dispatch tail-call.

    The generated switch must replace the literal PHA; otherwise the
    synthesized return address leaks onto the simulated SNES stack.
    """
    rom = make_lorom_bank0({
        0x8000: bytes([
            0xBD, 0x20, 0x0E,  # LDA $0E20,X
            0xC2, 0x30,        # REP #$30
            0x29, 0xFF, 0x00,  # AND #$00FF
            0x0A,              # ASL A
            0xA8,              # TAY
            0xB9, 0x20, 0x80,  # LDA $8020,Y
            0x3A,              # DEC A
            0x48,              # PHA ; dispatch site
            0xE2, 0x30,        # SEP #$30
            0x60,              # RTS
        ]),
        0x8020: bytes([
            0x00, 0x90,        # -> $9000
            0x00, 0x91,        # -> $9100
        ]),
        0x9000: bytes([0x60]),
        0x9100: bytes([0x60]),
    })

    src = emit_function(
        rom=rom,
        bank=0,
        start=0x8000,
        entry_m=1,
        entry_x=1,
        func_name='RtsStackDispatch',
        indirect_dispatch={
            0x00800E: {
                'count': 2,
                'idx_reg': 'Y',
                'table_bases': (0x8020,),
            },
        },
    )

    assert 'RTS-stack dispatch terminator: cfg-resolved target list' in src
    assert 'cpu->Y & 0xFFFF) / 2' in src
    assert 'cpu->P = (uint8)(cpu->P | 0x30)' in src
    assert 'bank_00_9000_M1X1(cpu)' in src
    assert 'bank_00_9100_M1X1(cpu)' in src
    assert 'bank_00_9000_M0X0(cpu)' not in src
    assert 'CPU_STACK_OP_PHA' not in src


def test_pha_rts_dispatch_missing_exact_target_uses_lle():
    rom = make_lorom_bank0({
        0x8000: bytes([0x48, 0xE2, 0x30, 0x60]),
        0x8020: bytes([0x00, 0x90, 0x00, 0x91]),
        0x9000: bytes([0x60]),
        0x9100: bytes([0x00]),
    })
    saved = codegen._VALID_VARIANTS
    saved_authoritative = codegen._VALID_VARIANTS_AUTHORITATIVE
    try:
        codegen.set_valid_variants({
            0x009000: frozenset({(1, 1)}),
        }, authoritative=True)
        src = emit_function(
            rom=rom, bank=0, start=0x8000,
            entry_m=1, entry_x=1,
            indirect_dispatch={
                0x008000: {
                    'count': 2, 'idx_reg': 'Y',
                    'table_bases': (0x8020,),
                },
            },
        )
    finally:
        codegen.set_valid_variants(
            saved, authoritative=saved_authoritative)

    assert 'bank_00_9000_M1X1(cpu)' in src
    assert 'bank_00_9100_M1X1(cpu)' not in src
    assert ('interp_tier_dispatch_tail(cpu, 0x009100u' in src
            and 'authoritative LLE M1X1' in src)


def test_pei_rts_stack_dispatch_is_same_function_goto_and_preserves_mx():
    rom = make_lorom_bank0({
        0x8000: bytes([
            0x8B,              # PHB
            0x5A,              # PHY (16-bit)
            0xE2, 0x20,        # SEP #$20 -> M1X0
            0xD4, 0x10,        # PEI ($10), DP contains target-1
            0x60,              # RTS (fused with PEI by the decoder)
        ]),
        0x8100: bytes([
            0xC2, 0x20,        # REP #$20 -> M0X0
            0x7A,              # PLY
            0xAB,              # PLB
            0x6B,              # RTL
        ]),
    })

    src = emit_function(
        rom=rom, bank=0, start=0x8000, entry_m=0, entry_x=0,
        indirect_dispatch={
            0x008004: {
                'count': 1,
                'idx_reg': 'X',
                'table_bases': (),
                'targets': (0x008100,),
                'rts_stack': True,
            },
        },
    )

    assert 'PEI(dp); RTS consumes target-1 as an internal goto' in src
    assert 'cpu->D + 0x0010' in src
    assert 'case 0x8100: goto L_8100_M1X0;' in src
    assert 'L_8100_M1X0:' in src
    assert 'CPU_STACK_OP_PEI' not in src
