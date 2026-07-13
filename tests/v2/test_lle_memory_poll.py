"""Automatic LLE tier-down for interrupt-owned memory polling loops."""

from v2.emit_function import emit_function


def _rom(code: bytes) -> bytes:
    return code + bytes(0x8000 - len(code))


def test_pure_memory_self_poll_unwinds_to_lle_interpreter():
    # PHP; SEP #$20; LDA $05B8; CMP $05B8; BEQ -5; PLP; RTS
    code = bytes.fromhex("08 E2 20 AD B8 05 CD B8 05 F0 FB 28 60")
    src = emit_function(_rom(code), bank=0, start=0x8000,
                        entry_m=0, entry_x=0, end=0x800D)

    assert "if (interp_bridge_in_lle_scheduler())" in src
    assert "interp_bridge_lle_yield_unwind(cpu, 0x008000u)" in src


def test_counter_loop_stays_compiled():
    # LDX #$03; DEX; BNE -3; RTS -- a finite CPU loop, not an interrupt poll.
    code = bytes.fromhex("A2 03 CA D0 FD 60")
    src = emit_function(_rom(code), bank=0, start=0x8000,
                        entry_m=1, entry_x=1, end=0x8006)

    assert "interp_bridge_lle_yield_unwind" not in src
