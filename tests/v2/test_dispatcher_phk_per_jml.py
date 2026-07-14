"""Yoshi-block freeze ROOT 2026-05-02: PHK+PER+JML inline-dispatch
trampoline.

The asm idiom at $00:BFBC (GenerateTile_Dispatch):
    SEP #$30
    LDA $9C        ; Map16TileGenerate
    DEC A
    PHK
    PER $0003
    JML ExecutePtr ; runtime dispatcher at $00:86DF
    .dw target0, target1, ...   ; 16-bit handler table

ExecutePtr consumes the PHK+PER bytes, indexes the table, and JML.W
[$00] tail-calls the handler. End-to-end the asm is balanced.

v2 bug (pre-fix): codegen emits PHK + PER as raw stack writes, then
returns NORMAL on the JML (because Goto-with-no-successor). The 3
trampoline bytes leak onto the simulated stack; later PLX/PLP/RTL
in the caller (GenerateTile) reads garbage and X cascades into
camera + sprite state — visible as Mario freezing on the Yoshi
block.

Fix: when a JML targets a registered dispatch helper (which the
classifier already detects), the codegen now:
  - skips emit for the immediately-preceding PHK/PEA/PER (trampoline
    setup that the runtime dispatcher would have consumed);
  - synthesizes a switch over `cpu->A & 0xFF` calling each handler
    by name — a tail-call dispatch, terminator returning NORMAL.
"""
from _helpers import make_lorom_bank0  # noqa: E402

from v2 import codegen  # noqa: E402
from v2.emit_function import emit_function  # noqa: E402


# ExecutePtr body (short, 16-bit table) ported byte-for-byte from
# $00:86DF. Decoder.classify_dispatch_helper recognizes it as 'short'.
EXECUTE_PTR_BYTES = bytes([
    0x84, 0x03,        # STY $03
    0x7A,              # PLY
    0x84, 0x00,        # STY $00
    0xC2, 0x30,        # REP #$30
    0x29, 0xFF, 0x00,  # AND #$00FF
    0x0A,              # ASL A
    0xA8,              # TAY
    0x68,              # PLA
    0x85, 0x01,        # STA $01
    0xC8,              # INY
    0xB7, 0x00,        # LDA [$00],Y
    0x85, 0x00,        # STA $00
    0xE2, 0x30,        # SEP #$30
    0xA4, 0x03,        # LDY $03
    0xDC, 0x00, 0x00,  # JML.W [$00]
])


def _build_rom_with_dispatcher():
    """Mini bank-0 image: ExecutePtr at $86DF + a 4-entry dispatcher
    at $BFBC + 4 dummy handlers at $C000/$C100/$C200/$C300."""
    blobs = {
        # ExecutePtr (target classified as 'short' dispatch helper)
        0x86DF: EXECUTE_PTR_BYTES,
        # Dispatcher at $BFBC: SEP #$30 / LDA $9C / DEC A / PHK / PER $0003 / JML $86DF
        0xBFBC: bytes([
            0xE2, 0x30,                 # SEP #$30
            0xA5, 0x9C,                 # LDA $9C
            0x3A,                       # DEC A
            0x4B,                       # PHK
            0x62, 0x03, 0x00,           # PER $0003
            0x5C, 0xDF, 0x86, 0x00,     # JML $00:86DF
            # 4-entry table at $BFC9
            0x00, 0xC0,                 # -> $C000
            0x00, 0xC1,                 # -> $C100
            0x00, 0xC2,                 # -> $C200
            0x00, 0xC3,                 # -> $C300
        ]),
        # Trivial handlers — each is just RTS so decode succeeds.
        0xC000: bytes([0x60]),  # RTS
        0xC100: bytes([0x60]),
        0xC200: bytes([0x60]),
        0xC300: bytes([0x60]),
    }
    return make_lorom_bank0(blobs)


def test_phk_per_jml_dispatcher_synthesizes_switch_and_skips_trampoline():
    """The dispatcher at $BFBC must emit a synthesized switch and skip
    the PHK + PER trampoline-setup pushes."""
    rom = _build_rom_with_dispatcher()

    # ExecutePtr lives at $00:86DF; v2_regen registers it via
    # classify_dispatch_helper. Pass the same map directly so emit
    # sees the JML target as a dispatcher.
    src = emit_function(
        rom=rom, bank=0, start=0xBFBC, entry_m=0, entry_x=0,
        func_name='GenerateTile_Dispatch',
        dispatch_helpers={0x0086DF: 'short'},
    )

    # The PHK + PER are skipped (with informational comments) — the
    # synthesized switch inlines their effect. If they were emitted as
    # raw cpu->S writes, the simulated stack would be corrupted by 3
    # bytes per dispatcher hit.
    assert 'trampoline setup PHK skipped' in src, (
        f'PHK trampoline-setup must be skipped; src=\n{src[:4000]}'
    )
    assert 'trampoline setup PER skipped' in src, (
        f'PER trampoline-setup must be skipped; src=\n{src[:4000]}'
    )

    # The synthesized switch comes from _emit_dispatch — its banner
    # comment is unique to that helper.
    assert 'JSL dispatch — short=2B / long=3B table' in src, (
        f'expected synthesized dispatch switch; src=\n{src[:4000]}'
    )
    # The switch indexes `cpu->A & 0xFF` (the 8-bit selector left in A
    # after SEP+LDA+DEC).
    assert '(uint16)(cpu->A & 0xFF)' in src or 'cpu->A & 0xFF' in src

    # Pre-fix gen ended with a literal PHK push: cpu_write8(cpu, 0x00,
    # cpu->S, (uint8)(cpu->PB)). After the fix the dispatcher's body
    # must not contain that raw PHK write, since PHK is skipped.
    assert 'cpu_write8(cpu, 0x00, cpu->S, (uint8)(cpu->PB))' not in src, (
        f'raw PHK write should have been skipped; src=\n{src[:4000]}'
    )


def test_phk_per_jml_dispatcher_decrements_S_zero_times():
    """Stack accounting check: the dispatcher body must NOT contain
    `cpu->S = (uint16)(cpu->S - 1)`. Every `S - 1` decrement comes
    from PHK / PEA / PER / push-helpers — all of which the trampoline
    skip removes. If a future change reintroduces a raw push in the
    dispatcher body, this test will catch it."""
    rom = _build_rom_with_dispatcher()
    src = emit_function(
        rom=rom, bank=0, start=0xBFBC, entry_m=0, entry_x=0,
        func_name='GenerateTile_Dispatch',
        dispatch_helpers={0x0086DF: 'short'},
    )
    # Count S-decrement statements outside the call_with_pb_save
    # block (which has its own bracketed scope per case body but does
    # NOT touch cpu->S — only cpu->PB).
    decrements = src.count('cpu->S = (uint16)(cpu->S - 1)')
    assert decrements == 0, (
        f'dispatcher must not push to simulated S after fix; '
        f'found {decrements} decrement(s)\n{src[:6000]}'
    )


def test_phk_per_jml_dispatcher_calls_handlers_by_name():
    """The synthesized switch must invoke each table entry's handler
    function. Without name resolution wiring (set_name_resolver), each
    handler is referenced by its synthetic `bank_00_C000`-style name."""
    rom = _build_rom_with_dispatcher()
    src = emit_function(
        rom=rom, bank=0, start=0xBFBC, entry_m=0, entry_x=0,
        func_name='GenerateTile_Dispatch',
        dispatch_helpers={0x0086DF: 'short'},
    )
    # Synthesized handler call shape: "bank_00_C000_M1X1(cpu)".
    # Suffix is _M1X1 because the JML insn's m_flag/x_flag = (1,1) after
    # the SEP #$30 prologue.
    for handler_pc in (0xC000, 0xC100, 0xC200, 0xC300):
        sym = f'bank_00_{handler_pc:04X}_M1X1(cpu)'
        assert sym in src, (
            f'expected handler call {sym} in synthesized dispatch; '
            f'src=\n{src[:6000]}'
        )


def test_phk_per_jml_dispatcher_tiers_manifest_lle_target_down():
    """An authoritative NULL AOT slot is executable ROM, not a link error.

    ExecutePtr forces M1X1, so a table entry without that exact body must run
    through the balanced tail-dispatch bridge.  It may not reference the
    absent C symbol or borrow another width variant.
    """
    rom = _build_rom_with_dispatcher()
    saved_variants = codegen._VALID_VARIANTS
    saved_authoritative = codegen._VALID_VARIANTS_AUTHORITATIVE
    try:
        codegen.set_valid_variants({
            0x00C000: frozenset({(1, 1)}),
            # C100 is intentionally absent => authoritative LLE-only.
            0x00C200: frozenset({(1, 1)}),
            0x00C300: frozenset({(1, 1)}),
        }, authoritative=True)
        src = emit_function(
            rom=rom, bank=0, start=0xBFBC, entry_m=0, entry_x=0,
            func_name='GenerateTile_Dispatch',
            dispatch_helpers={0x0086DF: 'short'},
        )
    finally:
        codegen.set_valid_variants(
            saved_variants, authoritative=saved_authoritative)

    assert 'bank_00_C000_M1X1(cpu)' in src
    assert 'bank_00_C100_M1X1(cpu)' not in src
    assert (
        'interp_tier_dispatch_tail(cpu, 0x00c100u, 0x00bfc5u, '
        '_entry_s, _hrv)' in src
    ), src
    assert 'authoritative LLE M1X1' in src
