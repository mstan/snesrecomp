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
    'test_decompress',
    'test_framework_fixes',
    'test_function_boundaries',
    'test_livein_inference',
    'test_sig_augment',
    'test_sync_funcs_h',
    'test_smwdisx_compare',
    'test_mflag_width',
    'test_emit_order_fallthrough',
    'test_dp_indirect_uses_db',
    'test_dp_alias_after_register_mutation',
    'test_side_effecting_lda_branch',
]


def main() -> int:
    sys.path.insert(0, str(TESTS_DIR))
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
