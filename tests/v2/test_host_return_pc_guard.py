"""Regression coverage for guest code that rewrites an RTS return frame."""

from _helpers import make_lorom_bank0  # noqa: E402

from v2.emit_function import emit_function  # noqa: E402


def test_rewritten_balanced_rts_dispatches_guest_return_pc():
    """Equal stack depth is insufficient when guest code rewrites the PC."""
    rom = make_lorom_bank0({
        0x8000: bytes([0x68,       # PLA: pop the JSR return address
                       0x3A,       # DEC A: change it while preserving depth
                       0x48,       # PHA
                       0x60]),     # RTS
    })

    src = emit_function(rom, bank=0, start=0x8000,
                        entry_m=0, entry_x=0,
                        func_name='RewriteReturn')

    assert 'uint32 _host_return_pc24 = 0xFFFFFFFFu;' in src, src
    assert '_rpc24 == _host_return_pc24' in src, src
    assert ('if (_hrv == 2 && _ret_s == _entry_s && '
            '_rpc24 == _host_return_pc24)') in src, src
    assert '_rpc24 != _host_return_pc24 && !cpu_dispatch_has_entry' in src, src
    assert 'interp_tier_dispatch_rewritten_return(cpu, _rpc24' in src, src
    assert 'cpu_dispatch_pc_from(cpu, _rpc24' in src, src


def test_deeper_computed_rts_unknown_target_tiers_into_interpreter():
    """PEI;RTS command dispatch must not look like a normal return miss.

    DKC2's decompressor keeps PHB/PHY locals on the stack, pushes a command
    address with PEI, and uses RTS as a computed jump. The command targets are
    data-driven and need not be static CFG roots. If an unknown target takes
    cpu_dispatch_pc_from's ordinary miss path, the routine returns early with
    DB/M/Y and S unrestored.
    """
    rom = make_lorom_bank0({
        0x8000: bytes([
            0x8B,        # PHB: saved local state makes S deeper than entry
            0xD4, 0x10,  # PEI ($10): synthetic computed-RTS frame
            0x60,        # RTS
        ]),
    })

    src = emit_function(rom, bank=0, start=0x8000,
                        entry_m=0, entry_x=0,
                        func_name='ComputedRtsWithSavedFrame')

    tier = src.index('interp_tier_dispatch_popped_return(cpu, _rpc24')
    miss = src.index('return cpu_dispatch_pc_from(cpu, _rpc24')
    assert '(uint16)(_entry_s - _ret_s) < 0x8000u' in src, src
    assert '!cpu_dispatch_has_entry(cpu, _rpc24)' in src, src
    assert tier < miss, src
