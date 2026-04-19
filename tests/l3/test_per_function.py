"""L3 per-function behavioral-equivalence tests.

Each test loads a savestate fixture on both recomp and oracle, invokes
a single recompiled function, and asserts that the resulting WRAM/VRAM
state matches between the two runtimes.

Green tests are regression guards — we already fixed the bug and want
it to stay fixed. @known_red tests pin currently-broken bugs so they
stay visible until someone fixes them (the decorator expects them to
FAIL; if they unexpectedly pass, it raises to tell you to remove the
marker).
"""
import sys
import pathlib

THIS_DIR = pathlib.Path(__file__).parent
sys.path.insert(0, str(THIS_DIR))

from harness import run_func, known_red, format_diff_summary  # noqa: E402


def test_HandleLevelTileAnimations_matches_emulator():
    """Regression guard for the 3N-multiplier codegen bug fixed in
    snesrecomp 252a2a0 (STA _0 ; ASL A ; ADC _0 folding the stale
    post-shift A back into ADC). Before the fix this function wrote
    $0101 to $0D7C/7E/80 instead of reading the correct entries from
    the $05:B93B pointer table."""
    diff = run_func(
        name='HandleLevelTileAnimations',
        fixture='attract_f94.snap',
        emu_ret='rts',
    )
    assert not diff, (
        f"HandleLevelTileAnimations diverges from emulator:\n"
        f"{format_diff_summary(diff)}"
    )


@known_red(
    "BG1 chr upload shortfall; recomp writes fewer VRAM words than "
    "emulator during attract-mode level-graphics load. Visible in-game "
    "as missing ground tiles and single-column bush rendering on the "
    "title-screen backdrop. Root cause not yet identified — pinned here "
    "for investigation"
)
def test_UploadPlayerGFX_matches_emulator():
    """Currently RED: at the captured attract-mode frame, UploadPlayerGFX
    produces ~211 fewer VRAM-word writes on recomp vs the emulator,
    concentrated in BG1 chr ($0600-$07FF) and OBJ chr ($1E00-$1FFF)
    regions. Test is marked @known_red until the underlying miscompile
    is found and fixed."""
    diff = run_func(
        name='UploadPlayerGFX',
        fixture='attract_f94.snap',
        emu_ret='rts',
    )
    assert not diff, (
        f"UploadPlayerGFX diverges from emulator:\n"
        f"{format_diff_summary(diff)}"
    )


if __name__ == '__main__':
    # Manual invocation for ad-hoc runs. Proper run is via run_tests.py.
    for fn_name in ('test_HandleLevelTileAnimations_matches_emulator',
                    'test_UploadPlayerGFX_matches_emulator'):
        fn = globals()[fn_name]
        try:
            fn()
            print(f'PASS  {fn_name}')
        except AssertionError as e:
            print(f'FAIL  {fn_name}: {e!s}'[:500])
