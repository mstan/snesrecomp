"""L3 harness sanity tests.

test_load_state_is_identical: both sides should load the same fixture
into identical state. If this fails, load_state has a bug and no other
L3 test will give meaningful results.
"""
import sys
import pathlib

THIS_DIR = pathlib.Path(__file__).parent
sys.path.insert(0, str(THIS_DIR))

from harness import run_load_only, format_diff_summary  # noqa: E402


def test_load_state_is_identical():
    diff = run_load_only('attract_f94.snap')
    assert not diff, (
        f"Load-only sanity: recomp and oracle disagree after loading the "
        f"same fixture. This must pass before any invoke-based L3 test can "
        f"give meaningful results.\n{format_diff_summary(diff)}"
    )


if __name__ == '__main__':
    try:
        test_load_state_is_identical()
        print('PASS  test_load_state_is_identical')
    except AssertionError as e:
        print(f'FAIL  test_load_state_is_identical:\n{e}'[:2000])
