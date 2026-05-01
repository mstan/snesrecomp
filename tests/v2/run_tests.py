"""Phase-gate runner for v2 tests.

Each test module under tests/v2/ exposes top-level test_* functions.
This driver imports them, runs each, and prints a summary. Exit 0 on
all-pass, 1 on any failure.

Run:  python snesrecomp/tests/v2/run_tests.py
"""
import importlib
import pathlib
import sys
import traceback

THIS = pathlib.Path(__file__).resolve().parent
REPO = THIS.parent.parent
sys.path.insert(0, str(REPO / 'recompiler'))
sys.path.insert(0, str(THIS))


TEST_MODULES = [
    'test_decoder_mode_split',
    'test_decoder_repsep_independent_bits',
    'test_decoder_immediate_length_per_state',
    'test_cfg_mode_split_blocks',
    'test_lowering_coverage',
    'test_lowering_per_op_smoke',
    'test_codegen_per_op_smoke',
    'test_emit_function_smoke',
    'test_emit_bank_smoke',
    'test_cfg_loader',
    'test_v2_emit_lint',
]


def main() -> int:
    failed = 0
    total = 0
    for mod_name in TEST_MODULES:
        mod = importlib.import_module(mod_name)
        for attr in dir(mod):
            if not attr.startswith('test_'):
                continue
            fn = getattr(mod, attr)
            if not callable(fn):
                continue
            total += 1
            try:
                fn()
                print(f"  PASS  {mod_name}.{attr}")
            except Exception:
                failed += 1
                print(f"  FAIL  {mod_name}.{attr}")
                traceback.print_exc()
    print()
    print(f"{total - failed}/{total} passed")
    return 0 if failed == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
