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


def test_indirect_jsr_ptrcall_switches_on_loaded_pointer_value():
    """JSR ($abs,X) can call through descriptor pointers, not table indexes."""
    rom = make_lorom_bank0({
        0x8000: bytes([
            0xA2, 0x00, 0x81,  # LDX #$8100
            0xFC, 0x00, 0x00,  # JSR ($0000,X)
            0xE6, 0x20,        # INC $20  ; continuation after handler RTS
            0x60,              # RTS
        ]),
        0x8100: bytes([0x00, 0x90]),  # descriptor field -> $9000
        0x9000: bytes([0x60]),        # RTS
    })

    src = emit_function(
        rom=rom,
        bank=0,
        start=0x8000,
        entry_m=1,
        entry_x=0,
        func_name='IndirectJsrPointerCall',
        indirect_dispatch={
            0x008003: {
                'count': 1,
                'idx_reg': 'X',
                'ptr_call': True,
                'targets': (0x9000,),
            },
        },
    )

    assert 'indirect dispatch pointer-call (JSR (abs,X))' in src
    assert 'uint16 _ptr = (uint16)(0x0000 + (cpu->X & 0xFFFF));' in src
    assert 'switch (_target)' in src
    assert 'case 0x9000:' in src
    assert 'cpu->X & 0xFFFF) / 2' not in src
    assert 'cpu->D + 0x0020' in src, src


def test_pea_jmp_ptrcall_resumes_at_pea_return_address():
    """PEA+JMP ptrcalls resume at the PEA'd RTS target, not after the JMP."""
    rom = make_lorom_bank0({
        0x8000: bytes([
            0xC2, 0x30,        # REP #$30
            0xA9, 0x00, 0x90,  # LDA #$9000
            0x85, 0x12,        # STA $12
            0xE6, 0x20,        # INC $20  ; PEA return label
            0xF4, 0x06, 0x80,  # PEA $8006 -> handler RTS returns to $8007
            0x6C, 0x12, 0x00,  # JMP ($12)
            0xE6, 0x21,        # lexical fall-through, not the RTS target
            0x60,              # RTS
        ]),
        0x9000: bytes([0x60]),  # RTS
    })

    src = emit_function(
        rom=rom,
        bank=0,
        start=0x8000,
        entry_m=1,
        entry_x=1,
        func_name='PeaJmpPointerCallLoop',
        indirect_dispatch={
            0x00800C: {
                'count': 1,
                'idx_reg': 'X',
                'ptr_call': True,
                'targets': (0x9000,),
            },
        },
    )

    assert 'indirect dispatch ptr-call (PEA+JMP idiom)' in src
    start = src.index('indirect dispatch ptr-call')
    snippet = src[start:start + 2000]
    assert 'goto L_8007_M0X0' in snippet, src
    assert 'goto L_9000_M0X0' not in snippet, src


def test_pea_jmp_long_ptrcall_switches_on_loaded_long_pointer():
    """PEA+JMP [$dp] ptrcalls switch on a 24-bit runtime pointer."""
    rom = make_lorom_bank0({
        0x8000: bytes([
            0xC2, 0x30,        # REP #$30
            0xA9, 0x00, 0x90,  # LDA #$9000
            0x85, 0x12,        # STA $12
            0xE2, 0x20,        # SEP #$20
            0xA9, 0x00,        # LDA #$00
            0x85, 0x14,        # STA $14
            0xC2, 0x20,        # REP #$20
            0xF4, 0x14, 0x80,  # PEA $8014 -> handler RTS returns to $8015
            0xDC, 0x12, 0x00,  # JMP [$12]
            0xE6, 0x20,        # INC $20 ; continuation after handler RTS
            0x60,              # RTS
        ]),
        0x9000: bytes([0x60]),  # RTS
    })

    src = emit_function(
        rom=rom,
        bank=0,
        start=0x8000,
        entry_m=1,
        entry_x=1,
        func_name='PeaJmpLongPointerCall',
        indirect_dispatch={
            0x008012: {
                'count': 1,
                'idx_reg': 'X',
                'ptr_call': True,
                'targets': (0x009000,),
            },
        },
    )

    assert 'absolute long-indirect dispatch: switch on the loaded pointer' in src
    assert 'case 0x009000:' in src
    assert 'PEA+JMP indirect call, 2-byte PEA frame' in src
    assert 'fall through to post-dispatch block' in src
    assert 'cpu->D + 0x0020' in src, src


def test_pea_jmp_finish_handler_resolves_ancestor_before_dispatch_hit():
    """A PEA+JMP-selected finish handler may PLA the PEA frame, then RTL.

    Super Metroid's room-state selector does this for the terminal state:
    the popped PC is the outer LoadRoomHeader continuation, which is also a
    valid dispatch-table entry. The stack level must win; otherwise codegen
    dispatches that continuation, gets NORMAL back, and resumes the selector
    loop against garbage room data.
    """
    rom = make_lorom_bank0({
        0x9000: bytes([
            0x68,  # PLA: consume PEA return frame
            0x6B,  # RTL: return to caller's caller
        ]),
    })

    src = emit_function(
        rom=rom,
        bank=0,
        start=0x9000,
        entry_m=0,
        entry_x=0,
        func_name='PeaJmpFinishHandler',
    )

    ancestor = src.index('cpu_resolve_ancestor_skip(_ret_s)')
    dispatch = src.index('return cpu_dispatch_pc_from')
    assert ancestor < dispatch, src
    assert 'if (_ret_s != _entry_s && !cpu_dispatch_has_entry' not in src
