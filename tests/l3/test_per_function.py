"""L3 per-function behavioral-equivalence tests, input-injection style.

Each test declares a minimal input contract (WRAM bytes + CPU regs),
invokes the function on both the recompiled binary (direct C call) and
the oracle (65816 interpreter, same ROM), and diffs a declared output
region. No savestate dependency — the inputs ARE the fixture.

Green tests are regression guards for bugs we fixed. @known_red tests
pin bugs we haven't — the decorator raises if an expected-red test
unexpectedly passes, prompting you to graduate it to green.
"""
import sys
import pathlib

THIS_DIR = pathlib.Path(__file__).parent
sys.path.insert(0, str(THIS_DIR))

from harness import run_stubbed, known_red, format_stubbed_diff  # noqa: E402


def test_HandleLevelTileAnimations_three_times_pattern():
    """Pin the 3N multiplier fix (snesrecomp 252a2a0).

    ROM at $05:BB3B does: LDA EffFrame ; AND #$07 ; STA _0 ; ASL A ; ADC _0
    expecting A = 3 * (EffFrame & 7) — used as a 6-byte-stride index
    into the $05:B93B pointer table. The function writes three words to
    $0D7C/7E/80 (Gfx33DestAddr C/B/A).

    Before the fix, ADC _0 folded to the post-ASL A variable, computing
    4N instead of 3N and reading misaligned entries. The observable was
    $0D7C/7E/80 = $0101 (garbage) instead of the correct table words.

    With EffFrame=3, we expect table entries at offset 3*3*2 = 18 = $12
    from the 3 base tables ($05:B93B/B93D/B93F).
    """
    diffs = run_stubbed(
        name='HandleLevelTileAnimations',
        inputs={0x0014: 0x03},       # EffFrame
        cpu={'db': 0x00, 'dp': 0x0000, 'pb': 0x05, 'e': 0, 'p': 0x30},
        # Writes both $D7C/7E/80 (Gfx33DestAddr C/B/A) and $D76/78/7A
        # (the loop body's GetAnimatedTile storage). Full output window
        # is $D76..$D81, 12 bytes.
        expected_writes=[(0x0D76, 12)],
        emu_ret='rtl',
    )
    assert not diffs, (
        f'HandleLevelTileAnimations(EffFrame=3) output diverges:\n'
        f'{format_stubbed_diff(diffs)}'
    )


def test_HandleLevelTileAnimations_eff_frame_zero():
    """EffFrame=0 is the degenerate case: 3*0=0, reads the first entry
    of each table. Good coverage for 'does the function work when
    EffFrame & 7 is zero' — an edge the 3N bug wouldn't have caught
    (0*3 == 0*4)."""
    diffs = run_stubbed(
        name='HandleLevelTileAnimations',
        inputs={0x0014: 0x00},
        cpu={'db': 0x00, 'dp': 0x0000, 'pb': 0x05, 'e': 0, 'p': 0x30},
        # Writes both $D7C/7E/80 (Gfx33DestAddr C/B/A) and $D76/78/7A
        # (the loop body's GetAnimatedTile storage). Full output window
        # is $D76..$D81, 12 bytes.
        expected_writes=[(0x0D76, 12)],
        emu_ret='rtl',
    )
    assert not diffs, format_stubbed_diff(diffs)


if __name__ == '__main__':
    tests = [
        'test_HandleLevelTileAnimations_three_times_pattern',
        'test_HandleLevelTileAnimations_eff_frame_zero',
    ]
    for tname in tests:
        fn = globals()[tname]
        try:
            fn()
            print(f'PASS  {tname}')
        except AssertionError as e:
            print(f'FAIL  {tname}:\n{str(e)[:2000]}')
