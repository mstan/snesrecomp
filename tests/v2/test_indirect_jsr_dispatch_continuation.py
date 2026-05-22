"""Regression test for JSR (abs,X) dispatch continuation semantics."""

from _helpers import make_lorom_bank0  # noqa: E402

from v2.emit_function import emit_function  # noqa: E402


def test_indirect_jsr_dispatch_falls_through_after_selected_handler():
    """A JSR (abs,X) jump table is a call, not a tail dispatch.

    ALttP attract sequence 4 uses:
        ASL A; TAX; JSR ($table,X); ...continue animation logic...

    The selected case routine RTSes back to the continuation. Codegen must
    therefore break out of the switch and keep emitting the following code,
    not return from the enclosing function as JMP/JML dispatches do.
    """
    rom = make_lorom_bank0({
        0x8000: bytes([
            0xA5, 0x10,        # LDA $10
            0x0A,              # ASL A
            0xAA,              # TAX
            0xFC, 0x00, 0x81,  # JSR ($8100,X)
            0xE6, 0x20,        # INC $20  ; must remain reachable
            0x60,              # RTS
        ]),
        0x8100: bytes([
            0x00, 0x90,        # -> $9000
            0x10, 0x90,        # -> $9010
        ]),
        0x9000: bytes([0x60]),  # RTS
        0x9010: bytes([0x60]),  # RTS
    })

    src = emit_function(
        rom=rom,
        bank=0,
        start=0x8000,
        entry_m=1,
        entry_x=1,
        func_name='IndirectJsrDispatchMidBlock',
        indirect_dispatch={
            0x008004: {
                'count': 2,
                'idx_reg': 'X',
            },
        },
    )

    assert 'indirect dispatch call: cfg-resolved target list' in src
    assert 'bank_00_9000_M1X1(cpu)' in src
    case0 = src[src.index('case 0:'):src.index('case 1:')]
    assert 'break;' in case0
    assert 'return RECOMP_RETURN_NORMAL;' not in case0
    assert src.count('cpu_trace_dispatch_oob') == 1
    assert 'cpu->D + 0x0020' in src, src
