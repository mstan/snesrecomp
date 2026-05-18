"""Pin emit_function's tail-call-past-end: codegen (2026-05-17 class fix).

When a function A has cfg `end:<pc>` AND a goto-or-fall-through edge
crosses that boundary into a target PC that resolves to a known
function entry in `codegen._NAME_RESOLVER`, emit a tail call:

    { RecompReturn _tc = B_M{m}X{x}(cpu); RecompStackPop(); return _tc; }

instead of `cpu_trace_unresolved_goto_trap(...)` (the prior emit). The
asm idiom (a function legitimately falls through into its declared
sibling) becomes a real C tail call, preserving both the function
boundary semantics declared by the cfg AND the asm's fall-through.

This is the recompiler-level fix the zelda3 ingest needed before it
could emit `func A end:<B>` instead of opaque `name` aliases — without
it the boundary edge fell into the unresolvable-goto trap, masking
the Intro_Init / Intro_Init_Continue control-flow (zelda3 root cause
2026-05-17). See project_zelda_intro_loop_root_2026_05_17.md.
"""
from _helpers import make_lorom_bank0  # noqa: E402

from v2.emit_function import emit_function  # noqa: E402
from v2 import codegen as v2_codegen  # noqa: E402


def _with_name_resolver(name_map):
    """Save+restore the codegen-global name resolver around a block."""
    class _Ctx:
        def __enter__(self):
            self.saved = dict(v2_codegen._NAME_RESOLVER)
            v2_codegen.set_name_resolver(name_map)
            return self

        def __exit__(self, *a):
            v2_codegen.set_name_resolver(self.saved)
    return _Ctx()


def test_fallthrough_past_end_into_named_sibling_emits_tail_call():
    """A: LDA #$01; STA $00; end:$8005. Linear, no terminator.
    B at $8005 is declared as 'IntroInitContinue' in the name resolver.
    Expected: A's fall-through past $8005 emits a tail call to
    IntroInitContinue_M1X1, not the unresolved-goto trap."""
    rom = make_lorom_bank0({
        # A: 5 bytes, no RTS — falls through past end:.
        0x8000: bytes([0xA9, 0x01, 0x85, 0x00, 0xEA]),
        # B: arbitrary body — only matters that it has a registered name.
        0x8005: bytes([0x60]),  # RTS
    })
    name_map = {
        (0x00 << 16) | 0x8005: 'IntroInitContinue',
    }
    with _with_name_resolver(name_map):
        src = emit_function(rom, bank=0, start=0x8000,
                            entry_m=1, entry_x=1,
                            end=0x8005,
                            func_name='IntroInit')

    # Tail-call to the sibling, with the boundary's (m, x) variant.
    assert 'IntroInitContinue_M1X1(cpu)' in src, src
    assert 'tail-call past end:' in src, src
    # Must NOT emit the unresolved-goto trap for that boundary.
    assert 'cpu_trace_unresolved_goto_trap' not in src, src


def test_fallthrough_past_end_records_call_demand_for_variant():
    """The tail-call site must register (target_pc24, m, x) demand so
    v2_regen's auto-promote pass synthesizes the right (m, x) variant
    of the sibling function. Otherwise the C linker can't find the
    symbol when the boundary's variant differs from the cfg-default."""
    rom = make_lorom_bank0({
        0x8000: bytes([0xC2, 0x30,  # REP #$30 → m=0, x=0
                       0xEA]),       # NOP (so we cross into B with m=0, x=0)
        0x8003: bytes([0x60]),       # B body
    })
    name_map = {
        (0x00 << 16) | 0x8003: 'NextSibling',
    }
    # Drain any prior demand so the assertion below is precise.
    v2_codegen.take_unresolved_call_targets()
    with _with_name_resolver(name_map):
        src = emit_function(rom, bank=0, start=0x8000,
                            entry_m=1, entry_x=1,
                            end=0x8003,
                            func_name='Caller')
    demand = v2_codegen.take_unresolved_call_targets()
    # The tail call lands on $00:8003 with the boundary's (m, x).
    # After REP #$30 + NOP, m=0 x=0 — the tail-call demand must reflect
    # that, NOT (1, 1).
    assert ((0x00 << 16) | 0x8003, 0, 0) in demand, demand
    assert 'NextSibling_M0X0(cpu)' in src, src


def test_jump_past_end_into_sibling_emits_tail_call_not_inline_import():
    """The intro-loop bug: BCS-into-sibling-function past end: was being
    inline-imported by the decoder (pulling the sibling's whole body
    into the source function's CFG). With sibling_entry_pcs passed, the
    decoder rejects the import and emit_function emits a tail-call.

    Synthetic ROM:
        A at $8000: BCS to $8010 ; STZ $00 ; RTS    (8 bytes)
        B at $8010: STZ $01 ; RTS                   (sibling — past A's end:$8010)

    With sibling_entry_pcs={0x8010}, A's emit must:
      - NOT contain L_8010_M1X1 (B not inline-imported)
      - Emit a tail-call to B at the BCS-taken path
    """
    rom = make_lorom_bank0({
        # A: BCS +3 → $8005 (past end:); STZ $00; RTS at $8004
        0x8000: bytes([0xB0, 0x03,  # BCS +3 → $8005 (past end:)
                       0x64, 0x00,  # STZ $00 (fall-through path)
                       0x60]),       # RTS
        0x8005: bytes([0x64, 0x01,  # STZ $01 (sibling B's body)
                       0x60]),       # RTS
    })
    name_map = {
        (0x00 << 16) | 0x8005: 'B',
    }
    with _with_name_resolver(name_map):
        # Without sibling_entry_pcs: decoder inlines B (the old behavior).
        src_inline = emit_function(rom, bank=0, start=0x8000,
                                   entry_m=1, entry_x=1,
                                   end=0x8005,
                                   func_name='A')
        # With sibling_entry_pcs={0x8005}: decoder refuses to inline.
        src_tail = emit_function(rom, bank=0, start=0x8000,
                                 entry_m=1, entry_x=1,
                                 end=0x8005,
                                 func_name='A',
                                 sibling_entry_pcs={0x8005})

    # Inline path imports B into A's CFG: B's RTS becomes a local label.
    assert 'L_8005_M1X1:' in src_inline, \
        f'expected inline-import without gate, got:\n{src_inline}'
    # Tail-call path keeps A small and routes BCS-taken through tail-call.
    assert 'L_8005_M1X1:' not in src_tail, \
        f'expected NO inline-import with gate, got:\n{src_tail}'
    assert 'B_M1X1(cpu)' in src_tail, src_tail
    assert 'tail-call past end:' in src_tail, src_tail


def test_unknown_target_still_traps():
    """Negative case: if the boundary's target has NO registered name,
    fall back to the unresolved-goto trap. (The class fix only fires
    when the target resolves to a named function entry — otherwise the
    trap is correct: there's literally no callable symbol for the
    sibling.)"""
    rom = make_lorom_bank0({
        0x8000: bytes([0xA9, 0x01, 0x85, 0x00, 0xEA]),
        0x8005: bytes([0x60]),
    })
    # Empty name resolver — no sibling registered.
    with _with_name_resolver({}):
        src = emit_function(rom, bank=0, start=0x8000,
                            entry_m=1, entry_x=1,
                            end=0x8005,
                            func_name='LoneFn')

    assert 'cpu_trace_unresolved_goto_trap' in src, src
    # No phantom tail-call code path.
    assert 'tail-call past end:' not in src, src
