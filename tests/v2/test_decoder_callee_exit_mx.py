"""Pin v2 decoder: callee exit-(m,x) propagates back to caller.

Reproduces the SMW $00:F465 / RunPlayerBlockCode bug at $00:ED80:
caller does `REP #$20` to set m=0, then `JSR <callee>` where the
callee internally does `SEP #$20` (m=1) and never restores. After
return, the caller's decoder must resume with m=1, not m=0.

Without callee_exit_mx propagation, the decoder mis-decodes operand
widths and can synthesise a phantom branch target inside a real
instruction's operand byte (the byte $02 = COP opcode), emitting a
malformed PHP/COP that drops a stack byte.

This test pins the structural fix: with callee_exit_mx populated,
post-JSR (m, x) reflects the callee's exit state.
"""
from _helpers import make_lorom_bank0  # noqa: E402

from v2.decoder import (  # noqa: E402
    decode_function, analyze_function_exit_mx, addr24, DecodeKey,
)


def _build_callee_sets_m_to_1():
    """Callee body at $9000: SEP #$20 then RTS. Exit-(m,x) = (1, x_unchanged)."""
    return {
        0x9000: bytes([
            0xE2, 0x20,   # SEP #$20  (sets m=1)
            0x60,         # RTS
        ]),
    }


def _build_caller_with_post_jsr_load():
    """Caller body at $8000:
        REP #$20         ; m=0
        JSR $9000        ; callee sets m=1
        LDA #$10 / STA $90  ; if decoder thinks m=0, consumes 3 bytes (LDA #$8590)
                            ; if decoder knows m=1, consumes 2 bytes (LDA #$10)
        STA $92          ; only reached cleanly when LDA was 2 bytes
        RTS
    """
    return {
        0x8000: bytes([
            0xC2, 0x20,         # REP #$20 (m=0)
            0x20, 0x00, 0x90,   # JSR $9000
            0xA9, 0x10,         # LDA #$10 (m=1 expected: 2 bytes)
            0x85, 0x90,         # STA $90
            0x85, 0x92,         # STA $92
            0x60,               # RTS
        ]),
    }


def _make_rom():
    blobs = {}
    blobs.update(_build_caller_with_post_jsr_load())
    blobs.update(_build_callee_sets_m_to_1())
    return make_lorom_bank0(blobs)


def test_post_jsr_decode_uses_callers_mx_when_no_map():
    """Without `callee_exit_mx`, decoder preserves caller's (m, x) across
    JSR — the legacy (buggy) behaviour, kept as default for back-compat.
    Verifies the bug is reproducible: at $8005 (post-JSR), the decoder
    decodes LDA in m=0 mode (3-byte LDA #imm)."""
    rom = _make_rom()
    graph = decode_function(rom, bank=0, start=0x8000, entry_m=1, entry_x=1)
    # Caller entered m=1, did REP #$20 (m=0), then JSR. Without exit-mx
    # propagation the post-JSR key has m=0, so LDA at $8005 gets decoded
    # at m=0 (LDA imm-16, length 3).
    post_jsr_keys = [k for k in graph.insns if k.pc == addr24(0, 0x8005)]
    assert post_jsr_keys, "post-JSR insn should be in graph"
    di = graph.insns[post_jsr_keys[0]]
    assert di.insn.mnem == 'LDA'
    assert post_jsr_keys[0].m == 0  # caller's m=0 used (BUG path)
    assert di.insn.length == 3      # 3-byte LDA #$8510 (BUG path)


def test_lle_first_analysis_stops_when_callee_exit_is_unproven():
    rom = _make_rom()
    graph = decode_function(
        rom, bank=0, start=0x8000, entry_m=1, entry_x=1,
        callee_exit_mx={}, stop_on_unknown_callee_exit=True)

    assert not any(k.pc == addr24(0, 0x8005) for k in graph.insns)
    assert graph.unknown_callee_exit_sites == [
        (addr24(0, 0x8002), addr24(0, 0x9000), 0, 1)
    ]


def test_post_jsr_decode_uses_callee_exit_mx_when_provided():
    """With `callee_exit_mx={(callee_pc24, em, ex): (1, x_in)}`, the
    decoder resumes with the callee's exit (m, x) so LDA at $8005 is
    decoded as 2-byte LDA #imm (m=1) — the correct width."""
    rom = _make_rom()
    callee_pc24 = addr24(0, 0x9000)
    # Caller calls $9000 with (m=0, x=1) post-REP. Callee exits with
    # (m=1, x=1). Map keyed by (target_pc24, entry_m, entry_x).
    callee_exit_mx = {(callee_pc24, 0, 1): (1, 1)}
    graph = decode_function(rom, bank=0, start=0x8000, entry_m=1, entry_x=1,
                            callee_exit_mx=callee_exit_mx)
    post_jsr_keys = [k for k in graph.insns if k.pc == addr24(0, 0x8005)]
    assert post_jsr_keys, "post-JSR insn should be in graph"
    di = graph.insns[post_jsr_keys[0]]
    assert di.insn.mnem == 'LDA'
    assert post_jsr_keys[0].m == 1   # callee's exit m=1 used (FIXED path)
    assert di.insn.length == 2       # 2-byte LDA #$10 (FIXED path)


def test_analyze_function_exit_mx_finds_uniform_exit():
    """A function with one terminator returns its (m, x) at that RTS."""
    rom = _make_rom()
    # Use the callee body at $9000 directly.
    graph = decode_function(rom, bank=0, start=0x9000, entry_m=0, entry_x=1)
    em, ex = analyze_function_exit_mx(graph)
    # SEP #$20 sets m=1 before RTS; x unchanged.
    assert em == 1
    assert ex == 1


def test_analyze_function_exit_mx_inherits_direct_tail_exit():
    rom = make_lorom_bank0({
        0x8000: bytes([0x4C, 0x00, 0x90]),  # JMP $9000
        0x9000: bytes([0x60]),              # RTS
    })
    graph = decode_function(
        rom, bank=0, start=0x8000, entry_m=1, entry_x=1,
        end=0x8003, sibling_entry_pcs={0x9000})

    assert analyze_function_exit_mx(
        graph, {(addr24(0, 0x9000), 1, 1): (0, 1)}) == (0, 1)


def test_analyze_function_exit_mx_returns_none_for_ambiguous():
    """A function with two terminators at different (m, x) returns None
    on the ambiguous component(s)."""
    # Build a body that branches: one path does SEP #$20 (m=1) before
    # RTS, the other goes straight to RTS (m unchanged).
    blobs = {
        0x8000: bytes([
            0x90, 0x03,         # BCC +3 (skip the SEP path on C=0)
            0xE2, 0x20,         # SEP #$20  (m=1)
            0x60,               # RTS  (path A, m=1)
            # Fall-through target after BCC: RTS  (path B, m=0)
            0x60,               # RTS
        ]),
    }
    rom = make_lorom_bank0(blobs)
    # Enter at m=0; one path stays m=0, the other exits m=1.
    graph = decode_function(rom, bank=0, start=0x8000, entry_m=0, entry_x=1)
    em, ex = analyze_function_exit_mx(graph)
    assert em is None  # ambiguous
    assert ex == 1     # uniform


def test_analyze_function_exit_mx_returns_none_for_no_terminators():
    """A function with no RTS/RTL/RTI yields (None, None)."""
    blobs = {
        # Tight infinite loop — no terminator.
        0x8000: bytes([0x80, 0xFE]),  # BRA -2  (jumps to itself)
    }
    rom = make_lorom_bank0(blobs)
    graph = decode_function(rom, bank=0, start=0x8000, entry_m=1, entry_x=1)
    assert analyze_function_exit_mx(graph) == (None, None)
