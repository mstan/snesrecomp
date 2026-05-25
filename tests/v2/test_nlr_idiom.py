"""Phase 2 (2026-05-02): non-local-return idiom detection.

Tests that v2 codegen recognizes the asm "PLA*N then RTS"
"return-to-grandparent" idiom and lowers it as
`cpu->pending_skip = SKIP_N` + return-via-pending-skip rather than as
literal PLA semantics. See RecompReturn enum in cpu_state.h for ABI
context, and project_first_db_corruption_root_2026_05_02 in memory
for the bug that motivated this support.
"""
from _helpers import make_lorom_bank0  # noqa: E402

from v2.emit_function import emit_function  # noqa: E402


def test_single_block_pla_pla_rts_emits_skip_1():
    """PLA / PLA / RTS (one block) — emit a SKIP_1 return without
    literal PullReg(A) ops."""
    rom = make_lorom_bank0({
        # 8-bit A on entry (M1X1) so PLA pops 1 byte each.
        0x8000: bytes([
            0x68,        # PLA
            0x68,        # PLA
            0x60,        # RTS
        ]),
    })
    src = emit_function(rom, bank=0, start=0x8000, entry_m=1, entry_x=1)

    assert "RECOMP_RETURN_SKIP_1" in src, (
        f"expected SKIP_1 emit for PLA/PLA/RTS idiom; src=\n{src}"
    )
    # Function-LOCAL `_pending_skip`, not `cpu->pending_skip`.
    # NLR signaling is C control-flow state, not 65816 hardware state.
    assert "_pending_skip = RECOMP_RETURN_SKIP_1" in src
    assert "cpu->pending_skip" not in src
    assert "CPU_TR_NLR_DETECT" in src
    # Literal PLA semantics (cpu_read8 of stack into A) MUST NOT
    # appear in the NLR block — would consume ancestor stack data.
    pla_reads = src.count("cpu_read8(cpu, 0x00, cpu->S);")
    assert pla_reads == 0, (
        f"NLR idiom should not emit literal PLA reads; found {pla_reads}\n{src}"
    )


def test_multi_block_bne_to_pla_pla_then_work_then_rts():
    """The original SMW $01:A3CB shape: BNE to a sub-block that does
    PLA PLA + branches forward; tail block does work + RTS. The NLR
    sub-block must set pending_skip and fall through; the real-work
    tail must run unchanged; the tail's RTS reads pending_skip and
    returns SKIP_1.

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

    assert "RECOMP_RETURN_SKIP_1" in src, (
        f"expected SKIP_1 emit for multi-block PLA*N idiom; src=\n{src}"
    )
    assert "_pending_skip = RECOMP_RETURN_SKIP_1" in src
    assert "cpu->pending_skip" not in src
    assert "CPU_TR_NLR_DETECT" in src
    assert "cpu_read8(cpu, 0x00, cpu->S);" not in src, (
        f"NLR multi-block path leaked a literal PLA read\n{src}"
    )


def test_setup_ops_before_pla_pla_jmp_emits_skip_1_and_keeps_setup():
    """Yoshi-block 2026-05-02: $00:F005's L_F024 has the pattern
        STA $1DFC ; STA $7D ; STA $1406 ; PLA ; PLA ; JMP $EE35
    where $EE35 ends in RTS. The setup STAs are real game-state
    changes (sound + Yoshi knockoff state) — emitter MUST keep them.
    Only the two PLAs and the resulting SKIP_1 emit are the NLR
    handling. The JMP target is decoded INTO this function's CFG;
    its RTS picks up _pending_skip.

    Pre-fix bug: the NLR detector required PLAs at the START of the
    block, missed this pattern, and emitted both PLAs as literal
    cpu->S pops. Mario-on-Yoshi-vs-koopa-slope death root cause.
    """
    rom = make_lorom_bank0({
        # $8000: STA $1DFC ($8D FC 1D)        — setup #1
        # $8003: STA $7D    ($85 7D)          — setup #2
        # $8005: PLA        ($68)             — NLR
        # $8006: PLA        ($68)
        # $8007: JMP $800B  ($4C 0B 80)       — JMP into rts-ending tail
        # $800A: NOP        ($EA)             — unreachable
        # $800B: STA $72    ($85 72)          — setup #3 (in tail)
        # $800D: RTS        ($60)             — terminates with SKIP_1
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

    # NLR was detected: SKIP_1 set, no literal PLA reads.
    assert "RECOMP_RETURN_SKIP_1" in src, (
        f"expected SKIP_1 on PLA-PLA-JMP-to-RTS pattern; src=\n{src}"
    )
    assert "_pending_skip = RECOMP_RETURN_SKIP_1" in src
    pla_reads = src.count("cpu_read8(cpu, 0x00, cpu->S);")
    assert pla_reads == 0, (
        f"NLR PLAs MUST NOT be emitted as literal cpu->S pops; "
        f"found {pla_reads} read8 sites of stack\n{src[:6000]}"
    )

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


def test_unbalanced_pla_count_is_not_detected_as_nlr():
    """A SINGLE PLA before RTS is not the NLR idiom (unit=2 for RTS;
    a single PLA is just a regular ALU stack pop). Detector must NOT
    fire — emit literal PLA semantics."""
    rom = make_lorom_bank0({
        0x8000: bytes([
            0x68,        # single PLA
            0x60,        # RTS
        ]),
    })
    src = emit_function(rom, bank=0, start=0x8000, entry_m=1, entry_x=1)

    assert "RECOMP_RETURN_SKIP_1" not in src, (
        f"single PLA misdetected as NLR; src=\n{src}"
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
