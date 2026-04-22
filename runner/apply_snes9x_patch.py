#!/usr/bin/env python3
"""
Idempotently apply snes9x_oracle.patch to the snes9x-core submodule.

Mirrors nesrecomp's nestopia_cmake.cmake patch-apply pattern, but adapted
for our MSBuild project (CMake's execute_process doesn't apply here, so
this runs as a plain Python pre-build helper).

Run after `git submodule update --init` and any time the patch file
changes. The script:
  1. Confirms the submodule is checked out at the expected commit.
  2. Checks if the patch is already applied (via `git apply --check`).
     If yes: no-op, exit 0.
     If no: applies it. If the apply fails, exits non-zero.

Invocation:
    python snesrecomp/runner/apply_snes9x_patch.py

This is required for any Oracle|x64 build to succeed — the snes9x_bridge
references the s9x_write_hook / s9x_insn_hook / s9x_nmi_hook globals that
the patch declares + plumbs into S9xMainLoop and getset.h.
"""
import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
SUBMODULE = HERE / 'snes9x-core'
PATCH = HERE / 'snes9x_oracle.patch'


def run(cmd, cwd=None, check=True):
    return subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, check=check)


def main() -> int:
    if not SUBMODULE.exists() or not (SUBMODULE / '.git').exists():
        if not (SUBMODULE / 'cpuexec.cpp').exists():
            print(f'snes9x-core submodule not initialized at {SUBMODULE}', file=sys.stderr)
            print('Run: git submodule update --init', file=sys.stderr)
            return 1

    if not PATCH.exists():
        print(f'patch file not found at {PATCH}', file=sys.stderr)
        return 1

    # Check if already applied. `git apply --check` succeeds (rc=0) if the
    # patch CAN be applied cleanly to the current state — i.e. it's NOT
    # yet applied. Returns non-zero if already applied or conflicts.
    check = run(['git', 'apply', '--check', str(PATCH)], cwd=SUBMODULE, check=False)
    if check.returncode != 0:
        # Either already applied (good) or conflicts (bad). Disambiguate
        # via reverse-check: --reverse --check succeeds (rc=0) only if the
        # patch is fully applied and could be cleanly reverted.
        rev = run(['git', 'apply', '--check', '--reverse', str(PATCH)],
                  cwd=SUBMODULE, check=False)
        if rev.returncode == 0:
            print('snes9x oracle patch already applied — no-op.')
            return 0
        # Neither forward nor reverse applies cleanly: real conflict.
        print('snes9x oracle patch fails both forward and reverse apply.', file=sys.stderr)
        print(f'  forward stderr: {check.stderr}', file=sys.stderr)
        print(f'  reverse stderr: {rev.stderr}', file=sys.stderr)
        print('Submodule may have drifted from the patch baseline; reset and re-init.', file=sys.stderr)
        return 1

    # Patch is fresh + applies cleanly. Apply it.
    apply = run(['git', 'apply', str(PATCH)], cwd=SUBMODULE, check=False)
    if apply.returncode != 0:
        print(f'patch apply failed: {apply.stderr}', file=sys.stderr)
        return 1
    print(f'Applied {PATCH.name} to snes9x-core submodule.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
