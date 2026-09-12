"""The desktop host's pacing clock (runner/src/desktop/host_clock.c).

The clock moved up from SuperMetroidRecomp's sm_video.c with its tests; this
compiles tests/host/host_clock_test.c against the unit and runs it. Missing
toolchain resolves to SKIPPED rather than absent, as for the bridge harness.
"""
import os
import pathlib
import shutil
import subprocess
import tempfile

REPO = pathlib.Path(__file__).resolve().parents[2]
UNIT = REPO / 'runner' / 'src' / 'desktop' / 'host_clock.c'
TEST = REPO / 'tests' / 'host' / 'host_clock_test.c'


def _skip(reason: str) -> None:
    print(f"    SKIPPED: {reason}")


def test_host_clock_contract():
    """Compile and run the C test; a non-zero exit is this test failing."""
    assert UNIT.is_file(), f"missing unit: {UNIT}"
    assert TEST.is_file(), f"missing test: {TEST}"
    cc = shutil.which('gcc') or shutil.which('cc') or shutil.which('clang')
    if cc is None or os.name == 'nt':
        _skip("no C compiler on PATH")
        return
    with tempfile.TemporaryDirectory() as tmp:
        exe = pathlib.Path(tmp) / 'host_clock_test'
        build = subprocess.run(
            [cc, '-std=c11', '-O1', '-UNDEBUG', '-Wall', '-Wextra', '-Werror',
             '-I', str(UNIT.parent), str(TEST), str(UNIT), '-lm', '-o', str(exe)],
            capture_output=True, text=True, timeout=300)
        if build.returncode != 0:
            raise AssertionError(f"host_clock_test failed to build:\n{build.stderr[-2000:]}")
        run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=300)
        if run.returncode != 0:
            raise AssertionError(
                f"host_clock_test exited {run.returncode}\n{run.stdout[-2000:]}\n{run.stderr[-2000:]}")
