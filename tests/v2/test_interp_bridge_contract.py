"""The interp816 / interp_bridge C contract harness, registered so CI runs it.

tests/interp816/run.sh has existed and passed for a long time while nothing
invoked it. An unregistered test cannot fail, and a test that cannot fail is
not a test (recomp-ai-rules/PRINCIPLES.md, "Enforce the Rule in the Artifact").

The cost of that was measured: the scheduler-owner fix in interp_bridge.c --
the one that keeps Super Metroid from running garbage into
InvalidInterrupt_Crash when the player presses Start at the title -- was
dropped by a rebase, and the suite that covers it (S8d) did not run, so
nothing said so. This module is the registration.

Missing toolchain resolves to SKIPPED rather than absent: the harness is
plain C99 and needs a compiler the Python suite otherwise does not.
"""
import os
import pathlib
import shutil
import subprocess

REPO = pathlib.Path(__file__).resolve().parents[2]
RUN_SH = REPO / 'tests' / 'interp816' / 'run.sh'


def _skip(reason: str) -> None:
    print(f"    SKIPPED: {reason}")


def test_interp816_and_bridge_contract():
    """Run the C harness; a non-zero exit is this test failing."""
    if not RUN_SH.is_file():
        raise AssertionError(f"missing harness: {RUN_SH}")
    if os.name == 'nt':
        _skip("run.sh is a POSIX shell harness")
        return
    for tool in ('bash', 'gcc'):
        if shutil.which(tool) is None:
            _skip(f"{tool} not on PATH")
            return

    proc = subprocess.run(
        ['bash', str(RUN_SH)],
        cwd=str(REPO),
        capture_output=True,
        text=True,
        timeout=900,
    )
    if proc.returncode != 0:
        # The harness prints one line per failed CHECK; surface them rather
        # than the whole transcript, which is mostly passing "ok" lines.
        fails = [ln for ln in proc.stdout.splitlines()
                 if 'FAIL' in ln or 'RESULT' in ln]
        detail = '\n'.join(fails) or proc.stdout[-2000:]
        raise AssertionError(
            f"tests/interp816/run.sh exited {proc.returncode}\n{detail}\n"
            f"{proc.stderr[-1000:]}")
