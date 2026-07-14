"""Non-local returns under the LLE-first hardware-stack model.

PLA*N and RTS execute literally against the authoritative guest stack. After
the PLAs expose an ancestor return frame, RTS pops its real PC and the runtime
converts the hardware stack watermark into a host RecompReturn. This keeps AOT
and LLE behavior identical without recognizing game-specific idioms.
"""
from _helpers import make_lorom_bank0  # noqa: E402

from v2.emit_function import emit_function  # noqa: E402


def test_single_block_pla_pla_rts_uses_hardware_stack_unwind():
    """PLA / PLA / RTS executes literally, then resolves the ancestor."""
    rom = make_lorom_bank0({
        # 8-bit A on entry (M1X1) so PLA pops 1 byte each.
        0x8000: bytes([
            0x68,        # PLA
            0x68,        # PLA
            0x60,        # RTS
        ]),
    })
    src = emit_function(rom, bank=0, start=0x8000, entry_m=1, entry_x=1)

    assert src.count("CPU_STACK_OP_PLA") == 2
    assert "cpu_resolve_ancestor_skip" in src
    assert "RTS return-to-ancestor" in src
    assert "interp_bridge_has_direct_paired_bounce" in src
    assert "interp_tier_dispatch_rewritten_return" in src
    assert "RECOMP_RETURN_SKIP_1" not in src


def test_multi_block_bne_to_pla_pla_then_work_then_rts():
    """The original SMW $01:A3CB shape: BNE to a sub-block that does
    PLA PLA + branches forward; tail block does work + RTS. The NLR
    sub-block must perform the guest PLAs and fall through; the real-work
    tail must run unchanged; the tail's RTS resolves the exposed ancestor.

    Fixture: BCS-taken path goes through PLAs, normal path skips
    them; both reach the same tail.
    """
    rom = make_lorom_bank0({
        # $8000: BCS $8005    (B0 03)
        # $8002: NOP          (EA)
        # $8003: BRA $8008    (80 03)
        # $8005: PLA          (68)       NLR start
        # $8006: PLA          (68)
        # $8007: BRA $8008    (80 00)    fall through to tail
        # $8008: NOP          (EA)       tail real work
        # $8009: 60                       RTS
        0x8000: bytes([
            0xB0, 0x03,
            0xEA,
            0x80, 0x03,
            0x68,
            0x68,
            0x80, 0x00,
            0xEA,
            0x60,
        ]),
    })
    src = emit_function(rom, bank=0, start=0x8000, entry_m=1, entry_x=1)

    assert src.count("CPU_STACK_OP_PLA") == 2
    assert "cpu_resolve_ancestor_skip" in src
    assert "RTS return-to-ancestor" in src
    assert "RECOMP_RETURN_SKIP_1" not in src


def test_setup_ops_before_pla_pla_jmp_use_hardware_unwind_and_keep_setup():
    """Yoshi-block 2026-05-02: $00:F005's L_F024 has the pattern
        STA $1DFC ; STA $7D ; STA $1406 ; PLA ; PLA ; JMP $EE35
    where $EE35 ends in RTS. The setup STAs are real game-state
    changes (sound + Yoshi knockoff state) — emitter MUST keep them.
    The two PLAs expose the ancestor frame on the real guest stack. The JMP
    target is decoded into this function's CFG and its RTS resolves that
    frame, while all setup writes remain intact.
    """
    rom = make_lorom_bank0({
        # $8000: STA $1DFC ($8D FC 1D)        — setup #1
        # $8003: STA $7D    ($85 7D)          — setup #2
        # $8005: PLA        ($68)             — NLR
        # $8006: PLA        ($68)
        # $8007: JMP $800B  ($4C 0B 80)       — JMP into rts-ending tail
        # $800A: NOP        ($EA)             — unreachable
        # $800B: STA $72    ($85 72)          — setup #3 (in tail)
        # $800D: RTS        ($60)             — resolves exposed ancestor
        0x8000: bytes([
            0x8D, 0xFC, 0x1D,    # STA $1DFC
            0x85, 0x7D,          # STA $7D
            0x68,                # PLA
            0x68,                # PLA
            0x4C, 0x0B, 0x80,    # JMP $800B
            0xEA,                # NOP
            0x85, 0x72,          # STA $72   ← in target block
            0x60,                # RTS
        ]),
    })
    src = emit_function(rom, bank=0, start=0x8000, entry_m=1, entry_x=1)

    assert src.count("CPU_STACK_OP_PLA") == 2
    assert "cpu_resolve_ancestor_skip" in src
    assert "RTS return-to-ancestor" in src
    assert "RECOMP_RETURN_SKIP_1" not in src

    # Setup ops MUST be preserved — without these, the original
    # game-state changes (sound, Yoshi knockoff flags) would be lost.
    assert "0x1dfc" in src.lower() or "0x1DFC" in src, (
        f"setup STA $1DFC must be emitted before NLR PLAs; src=\n{src[:6000]}"
    )
    assert "0x007d" in src.lower(), (
        f"setup STA $7D must be emitted; src=\n{src[:6000]}"
    )
    assert "0x0072" in src.lower(), (
        f"target-block STA $72 must be emitted; src=\n{src[:6000]}"
    )


def test_single_pla_before_rts_keeps_literal_hardware_semantics():
    """A single PLA remains an ordinary guest-stack pop before RTS."""
    rom = make_lorom_bank0({
        0x8000: bytes([
            0x68,        # single PLA
            0x60,        # RTS
        ]),
    })
    src = emit_function(rom, bank=0, start=0x8000, entry_m=1, entry_x=1)

    assert "RECOMP_RETURN_SKIP_1" not in src, (
        f"single PLA should not use a synthetic SKIP constant; src=\n{src}"
    )
    assert "cpu_read8(cpu, 0x00, cpu->S);" in src, (
        f"literal PLA pop missing for non-NLR shape; src=\n{src}"
    )


def test_jsr_callsite_propagates_skip_n():
    """JSR callsite must check the callee's RecompReturn and propagate
    SKIP_N upward by returning SKIP_(N-1). Emit now uses a runtime
    (m, x) dispatch switch (2026-05-23) — propagation block sits after
    the switch."""
    rom = make_lorom_bank0({
        0x8000: bytes([
            0x20, 0x10, 0x80,   # JSR $8010
            0x60,               # RTS at $8003
        ]),
        0x8010: bytes([0x60]),  # RTS at $8010
    })
    src = emit_function(rom, bank=0, start=0x8000, entry_m=1, entry_x=1)

    assert "RecompReturn _r;" in src
    assert "RECOMP_RETURN_NORMAL" in src
    assert "(int)_r - 1" in src or "(int)_r-1" in src
    # All four (m, x) variants emitted as switch cases.
    for sfx in ("_M0X0", "_M0X1", "_M1X0", "_M1X1"):
        assert f"bank_00_8010{sfx}(cpu)" in src, (
            f"variant bank_00_8010{sfx} missing\n{src}"
        )


if __name__ == '__main__':
    import sys
    import traceback
    failed = 0
    for name in [n for n in dir() if n.startswith('test_')]:
        try:
            globals()[name]()
            print(f"PASS  {name}")
        except Exception:
            failed += 1
            print(f"FAIL  {name}")
            traceback.print_exc()
    sys.exit(1 if failed else 0)
