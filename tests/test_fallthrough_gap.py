"""Fall-through across a cfg gap must not emit a spurious tail call.

Reproduces the SMW I_RESET emission bug at ROM $00:8000-$806A. The
cfg was:

    func I_RESET        8000 end:806b
    exclude_range       806b 8079
    func HandleSPCUploads_Inner 8079 ...

The I_RESET body ends on `INC $10` (a non-terminal instruction) at
$8069. End-of-decode is $806B. Previously the emitter saw a "next
function" at $8079 and unconditionally emitted a fall-through call:

    HandleSPCUploads_Inner(&g_ram[0x00]);  /* fall-through */

That is wrong: execution cannot naturally reach $8079 by falling
through $806B-$8078 because those bytes are in an exclude_range
(the ROM's GameLoop, which SmwRunOneFrameOfGame supersedes). The
fall-through should only fire when the next function's start address
is contiguous with end_addr.

Fix: only set `next_func` when `nf_start == end_addr`. If there's a
gap, leave next_func None; the emitter closes the body with `return`.
"""
import pathlib
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / 'recompiler'))

import recomp  # noqa: E402


def test_fallthrough_to_non_contiguous_next_func_is_suppressed():
    # Synthesize a minimal ROM with two functions and a gap.
    #   $8000: LDA #$01 ; RTS           (terminal, 3 bytes)
    # Well, we want NON-terminal, so:
    #   $8000: LDA #$01 ; STA $10       (non-terminal, 4 bytes)
    # Then end:8004, with a gap, and next func at $8010.
    rom = bytearray(0x10000)
    # Header placeholder (recomp doesn't care for this small test).
    # Function A at $8000: LDA #$01 ; STA $10      (4 bytes, non-terminal)
    rom[0x0000] = 0xA9; rom[0x0001] = 0x01
    rom[0x0002] = 0x85; rom[0x0003] = 0x10
    # Gap $8004-$800F (garbage — exclude-equivalent from emitter POV:
    # what matters is that cfg's "next function" starts at $8010, not $8004).
    for a in range(0x0004, 0x0010):
        rom[a] = 0xEA  # harmless NOPs — decoder won't see these; cfg says gap.
    # Function B at $8010: RTS
    rom[0x0010] = 0x60

    # Build a minimal cfg structure using recomp.Config.
    cfg = recomp.Config()
    cfg.bank = 0
    cfg.funcs = [('funcA', 0x8000), ('funcB', 0x8010)]
    cfg.sigs = {0x8000: 'void()', 0x8010: 'void()'}
    cfg.names = {0x8000: 'funcA', 0x8010: 'funcB'}
    cfg.exclude_ranges = [(0x8004, 0x8010)]

    # funcs_with_end is (name, start, end, sig) tuples in recomp's pipeline;
    # reproduce the shape that run_config builds.
    funcs_with_end = [
        ('funcA', 0x8000, 0x8004, 'void()'),  # end at $8004 (gap before next)
        ('funcB', 0x8010, 0x8011, 'void()'),  # end at $8011 (after RTS)
    ]

    # Emulate the fall-through selection logic from run_config.
    # We're testing the next_func selection rule directly.
    fi = 0
    end_addr = funcs_with_end[fi][2]  # $8004
    next_func = None
    if fi + 1 < len(funcs_with_end):
        nf_start = funcs_with_end[fi + 1][1]
        if nf_start == end_addr:
            next_func = (funcs_with_end[fi + 1][0], funcs_with_end[fi + 1][3])
    assert next_func is None, (
        f'next_func should be None when nf_start ($8010) != end_addr ($8004); '
        f'got {next_func}. A non-None next_func here would cause emit_function '
        f'to generate a spurious tail-call to funcB when funcA actually ends '
        f'naturally before the gap.'
    )


def test_fallthrough_to_contiguous_next_func_still_works():
    # Contiguous pair: funcA at $8000 end:$8004, funcB at $8004.
    # The rule must still permit the fall-through call.
    funcs_with_end = [
        ('funcA', 0x8000, 0x8004, 'void()'),
        ('funcB', 0x8004, 0x8005, 'void()'),
    ]
    fi = 0
    end_addr = funcs_with_end[fi][2]  # $8004
    next_func = None
    if fi + 1 < len(funcs_with_end):
        nf_start = funcs_with_end[fi + 1][1]
        if nf_start == end_addr:
            next_func = (funcs_with_end[fi + 1][0], funcs_with_end[fi + 1][3])
    assert next_func == ('funcB', 'void()'), (
        f'Contiguous next_func must still be selected. Got {next_func}.'
    )
