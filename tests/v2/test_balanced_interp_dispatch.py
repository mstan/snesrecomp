from _helpers import make_lorom_bank0

from v2.emit_function import emit_function


def test_reserved_hle_dispatch_uses_balanced_interpreter_tier():
    rom = make_lorom_bank0({
        0x8000: bytes([0xDC, 0x00, 0x10]),  # JML [$1000]
    })

    src = emit_function(
        rom,
        bank=0,
        start=0x8000,
        entry_m=0,
        entry_x=0,
        hle_dispatch={0x8000: '__balanced_interp__'},
    )

    assert 'interp_tier_dispatch_balanced(cpu, 0x008000u, 0x008000u' in src
    assert '/* balanced_interp_dispatch */' in src
    assert 'RecompStackPop(); return _r;' in src
    assert 'extern RecompReturn __balanced_interp__' not in src


def test_reserved_dispatch_lowers_phk_pea_jml_as_pushed_call():
    rom = make_lorom_bank0({
        0x8000: bytes([
            0x4B,              # PHK
            0xF4, 0x06, 0x80,  # PEA $8006 -> RTL resumes at $8007
            0xDC, 0x84, 0x17,  # JML [$1784]
            0xEA,              # $8007 continuation
            0x6B,              # RTL
        ]),
    })

    src = emit_function(
        rom,
        bank=0,
        start=0x8000,
        entry_m=0,
        entry_x=0,
        hle_dispatch={0x8004: '__balanced_interp__'},
    )

    assert 'cpu_dispatch_call_pc_pushed(cpu' in src
    assert '0x1784' in src and '0x1786' in src
    assert '0x008004u, 3, &_disp_ret' in src
    assert 'case 0x8007u: goto L_8007_M0X0' in src


def test_reserved_dispatch_lowers_pea_jmp_as_pushed_near_call():
    rom = make_lorom_bank0({
        0x8000: bytes([
            0xF4, 0x05, 0x80,  # PEA $8005 -> RTS resumes at $8006
            0x6C, 0x9C, 0x09,  # JMP ($099C)
            0xEA,              # $8006 continuation
            0x60,              # RTS
        ]),
    })

    src = emit_function(
        rom,
        bank=0,
        start=0x8000,
        entry_m=0,
        entry_x=0,
        hle_dispatch={0x8003: '__balanced_interp__'},
    )

    assert '/* balanced pushed indirect near call */' in src
    assert 'cpu_read16(cpu, 0x00, 0x099c)' in src
    assert '((uint32)cpu->PB << 16) | _disp_lo' in src
    assert '0x008003u, 2, &_disp_ret' in src
    assert 'case 0x8006u: goto L_8006_M0X0' in src
