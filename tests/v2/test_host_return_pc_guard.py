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


def test_direct_bounce_computed_rts_returns_to_owning_interpreter():
    """A fully consumed synthetic frame must not start a nested bridge.

    PHA target-1; RTS leaves S exactly at the AOT entry depth while preserving
    the interpreted caller's real frame above it.  The continuation therefore
    belongs to the active interpreter, even though pre-RTS S was deeper than
    entry.  A generic popped-return tier would execute a later non-local caller
    epilogue once inside that nested tier and again in the owner.
    """
    rom = make_lorom_bank0({
        0x8000: bytes([0x48, 0x60]),  # PHA; RTS computed dispatch
    })
    src = emit_function(rom=rom, bank=0, start=0x8000,
                        entry_m=0, entry_x=0,
                        func_name='ComputedRtsDirectBounce')

    guard = src.index('cpu->S == _entry_s &&')
    rewritten = src.index('interp_tier_dispatch_rewritten_return(cpu, _rpc24',
                          guard)
    popped = src.index('interp_tier_dispatch_popped_return(cpu, _rpc24')
    assert 'interp_bridge_has_direct_paired_bounce()' in src[guard:rewritten], src
    assert guard < rewritten < popped, src


def test_partial_frame_return_uses_rewritten_return_bridge():
    rom = make_lorom_bank0({
        0x8000: bytes([0x8B, 0x60]),  # PHB; RTS crosses entry watermark
    })
    src = emit_function(rom=rom, bank=0, start=0x8000,
                        entry_m=0, entry_x=0,
                        func_name='PartialFrameReturn')
    partial = src.index('cpu->S - _entry_s')
    rewritten = src.index('interp_tier_dispatch_rewritten_return(cpu, _rpc24',
                          partial)
    popped = src.index('interp_tier_dispatch_popped_return(cpu, _rpc24')
    assert rewritten < popped, src


def test_jsr_bounce_that_returns_through_outer_rtl_yields_to_bridge():
    """Return ownership follows the stack, not the final opcode's frame size.

    A dispatcher can be entered by an interpreter-owned JSR, consume that
    two-byte frame, and tail into a shared RTL that returns through the
    interpreter's enclosing JSL frame. The final RTL therefore sees hrv=2.
    Its pre-pop S is shallower than the bounced entry S, which must yield the
    popped continuation to the owning interpreter instead of starting a new
    dispatch root.
    """
    rom = make_lorom_bank0({
        0x8000: bytes([0x6B]),  # RTL
    })
    src = emit_function(rom=rom, bank=0, start=0x8000,
                        entry_m=0, entry_x=0,
                        func_name='OuterFrameRtl')

    assert ('(uint16)(_ret_s - _entry_s) < 0x8000u' in src), src
    assert ('interp_bridge_has_direct_paired_bounce()' in src), src
    assert ('_hrv == 3 && interp_bridge_has_direct_paired_bounce()'
            not in src), src
