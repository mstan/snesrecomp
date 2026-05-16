"""Pin v2 decoder PHP/PLP M/X tracking.

PHP pushes the current (m, x) onto a per-key p_stack. PLP pops the top
entry and RESTORES (m, x) to that snapshot.

Without this tracking, the canonical SMW idiom

    PHX ; PHY ; PHP ; SEP #$30 ; … ; PLP ; PLY ; PLX ; RTS

de-syncs static-width pinning at the PLP-restored PLX/PLY:
- decoder otherwise stays at the post-SEP (m=1, x=1) state through PLP
- 1-byte push (entry x=0) then 2-byte pop (post-SEP x=1) → stack drift

Concrete failure: UpdateSaveBuffer at $04:9037 had -2 stack drift when
called with caller (m=0, x=0). The drift cascaded into overworld
artifacts (lives-counter sprite corruption, stage-entry mosaic stuck).
PHP/PLP tracking makes the post-PLP PLX/PLY decode at entry (m=0, x=0)
so the pop width matches the push width.
"""
from _helpers import make_lorom_bank0  # noqa: E402

from v2.decoder import decode_function, DecodeKey, post_state  # noqa: E402


def test_php_pushes_current_mx_onto_p_stack():
    """PHP records the current (m, x) for later PLP restore."""
    # Synthesize an Insn-like object.
    class _Insn:
        mnem = 'PHP'
        operand = 0
    m, x, ps = post_state(_Insn(), in_m=0, in_x=1, in_p_stack=())
    assert m == 0 and x == 1, "PHP doesn't modify (m, x); it pushes P"
    assert ps == ((0, 1),), "PHP pushes the current (m, x) onto p_stack"


def test_plp_restores_top_of_p_stack():
    """PLP pops the top of p_stack and restores (m, x) to it."""
    class _Insn:
        mnem = 'PLP'
        operand = 0
    m, x, ps = post_state(_Insn(), in_m=1, in_x=1, in_p_stack=((0, 0),))
    assert m == 0 and x == 0, "PLP restores (m, x) to the popped snapshot"
    assert ps == (), "PLP pops one entry off p_stack"


def test_plp_with_empty_p_stack_keeps_current_mx():
    """Unbalanced PLP (no matching PHP) keeps current (m, x) — conservative."""
    class _Insn:
        mnem = 'PLP'
        operand = 0
    m, x, ps = post_state(_Insn(), in_m=1, in_x=1, in_p_stack=())
    assert m == 1 and x == 1
    assert ps == ()


def test_sep_inside_php_plp_bracket_restores_on_plp():
    """Canonical PHP / SEP / PLP bracket: (m, x) reverts at PLP."""
    # Equivalent to:
    #   $8000: PHP           (08)
    #   $8001: SEP #$30      (E2 30)
    #   $8003: PLP           (28)
    #   $8004: RTS           (60)
    rom = make_lorom_bank0({
        0x8000: bytes([0x08, 0xE2, 0x30, 0x28, 0x60]),
    })
    graph = decode_function(rom, bank=0x00, start=0x8000,
                            entry_m=0, entry_x=0)
    # PLP at $8003: entry (m, x) at the PLP itself is post-SEP (1, 1).
    # AFTER PLP fires, p_stack pops and (m, x) reverts to (0, 0).
    # The successor key (RTS at $8004) gets the restored (0, 0).
    plp_key = DecodeKey(pc=0x008003, m=1, x=1, p_stack=((0, 0),))
    assert plp_key in graph.insns, (
        "PLP at $8003 must be decoded with the post-SEP state "
        "(m=1, x=1) and p_stack=((0, 0),) recording the pre-SEP snapshot"
    )
    # The RTS at $8004 must be decoded with the PLP-restored state.
    rts_key = DecodeKey(pc=0x008004, m=0, x=0, p_stack=())
    assert rts_key in graph.insns, (
        "RTS at $8004 must be at the PLP-restored (m=0, x=0) with empty p_stack"
    )


def test_updatesavebuffer_shape_plx_decoded_at_entry_state():
    """Reproduce UpdateSaveBuffer's PHX/PHY/PHP/SEP/.../PLP/PLY/PLX/RTS
    bracket. With PHP/PLP tracking, the PLX at end is decoded at the
    function's ENTRY (m, x), not the post-SEP state.
    """
    # $8000: PHX             (DA)
    # $8001: PHY             (5A)
    # $8002: PHP             (08)
    # $8003: SEP #$30        (E2 30)
    # $8005: NOP             (EA)        — body placeholder
    # $8006: PLP             (28)
    # $8007: PLY             (7A)
    # $8008: PLX             (FA)
    # $8009: RTS             (60)
    rom = make_lorom_bank0({
        0x8000: bytes([0xDA, 0x5A, 0x08, 0xE2, 0x30, 0xEA, 0x28, 0x7A, 0xFA, 0x60]),
    })
    # Entry M=0 X=0: the M0X0 variant of UpdateSaveBuffer.
    graph = decode_function(rom, bank=0x00, start=0x8000,
                            entry_m=0, entry_x=0)

    # PLY at $8007: must be decoded at entry (m=0, x=0) — the PLP just
    # before it restored those values. With OLD post_mx (no PHP/PLP
    # tracking), this would be decoded at (m=1, x=1) and emit a 1-byte
    # pop, mismatching the 2-byte PHY push.
    ply_key = DecodeKey(pc=0x008007, m=0, x=0, p_stack=())
    assert ply_key in graph.insns, (
        f"PLY at $8007 must be decoded at the PLP-restored (m=0, x=0). "
        f"Found keys at $8007: {[(k.m, k.x, k.p_stack) for k in graph.insns if k.pc == 0x008007]}"
    )
    assert graph.insns[ply_key].insn.mnem == 'PLY'

    # PLX at $8008: same — decoded at restored (m=0, x=0).
    plx_key = DecodeKey(pc=0x008008, m=0, x=0, p_stack=())
    assert plx_key in graph.insns
    assert graph.insns[plx_key].insn.mnem == 'PLX'


def test_php_plp_balanced_loop_doesnt_grow_p_stack():
    """A loop with balanced PHP/PLP shouldn't accumulate p_stack across
    iterations (the loop-back edge merges to the same key)."""
    # $8000: PHP            (08)
    # $8001: PLP            (28)
    # $8002: BRA $8000      (80 FC)
    rom = make_lorom_bank0({
        0x8000: bytes([0x08, 0x28, 0x80, 0xFC]),
    })
    graph = decode_function(rom, bank=0x00, start=0x8000,
                            entry_m=1, entry_x=1)
    # PHP at $8000 must be reached with p_stack=() (entry + every loop-back).
    php_key = DecodeKey(pc=0x008000, m=1, x=1, p_stack=())
    assert php_key in graph.insns, (
        "PHP at $8000 must be decoded with empty p_stack — both the entry "
        "edge and the BRA back-edge converge to the same key"
    )
    # No additional PHP variants with stack depth > 0 should exist
    # (the PLP at $8001 pops back to empty before the BRA fires).
    php_keys = [k for k in graph.insns if k.pc == 0x008000]
    assert len(php_keys) == 1, (
        f"Expected exactly 1 PHP variant at $8000, found {len(php_keys)}: "
        f"{[(k.m, k.x, k.p_stack) for k in php_keys]}"
    )
