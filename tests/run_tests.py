#!/usr/bin/env python3
"""
Regression test runner for the SNES recompiler.

Each test is a function in this directory that asserts an invariant about
either (a) decoder output — the byte-for-byte decompress oracle — or
(b) structural properties of emitted .c code for specific ROM regions.

Tests are expected to be cheap and repeatable. Anything that needs a real
runtime launch belongs in a separate integration harness.

Exit code: 0 all pass, 1 any fail.
"""
import subprocess
import sys
import importlib
import pathlib
import traceback

TESTS_DIR = pathlib.Path(__file__).parent
REPO_ROOT = TESTS_DIR.parent

TEST_MODULES = [
    # v2-applicable tests that survived the v1 trim. v1-specific tests
    # (recomp.py / src/gen/*_gen.c paths) were removed; their structural
    # cousins for v2 live under tests/v2/ and run via tests/v2/run_tests.py.
    'test_attract_demo_regression',
    'test_dispatch_extents',
    'test_emitter_mask_shape',
    'test_smwdisx_compare',
    'test_sync_funcs_h',
]


def main() -> int:
    sys.path.insert(0, str(TESTS_DIR))
    # Width-mask DRY lint runs first — load-bearing gate. Fast-fails the
    # test loop if any new emitter slipped raw width literals past the
    # widths.py chokepoint. Plan source: DRY_REFACTOR.md Step 3.
    lint_path = REPO_ROOT / 'tools' / 'lint_codegen_widths.py'
    if lint_path.exists():
        rc = subprocess.call([sys.executable, str(lint_path)])
        if rc != 0:
            print('lint_codegen_widths failed — aborting test loop')
            return rc
    passed = 0
    failed = 0
    fail_log = []
    for modname in TEST_MODULES:
        mod = importlib.import_module(modname)
        tests = [(n, getattr(mod, n)) for n in dir(mod) if n.startswith('test_')]
        for name, fn in tests:
            label = f'{modname}.{name}'
            try:
                fn()
                print(f'  PASS  {label}')
                passed += 1
            except AssertionError as e:
                print(f'  FAIL  {label}: {e}')
                fail_log.append((label, str(e)))
                failed += 1
            except Exception:
                print(f'  ERR   {label}')
                tb = traceback.format_exc()
                fail_log.append((label, tb))
                failed += 1
    print()
    print(f'{passed} passed, {failed} failed')
    if fail_log:
        print()
        for label, msg in fail_log:
            print(f'--- {label} ---')
            print(msg)
    return 0 if failed == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
