"""Demo: monkey-patch Reg.B back to a field shadow, verify the lint
flags it. Doesn't modify the repo — only the in-process import."""
import sys, pathlib
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / 'tests' / 'v2'))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / 'recompiler'))

from v2 import codegen, ir
import test_v2_emit_lint as t

# Re-introduce the regression: point Reg.B at a literal cpu->B field.
codegen._REG_FIELD[ir.Reg.B] = "cpu->B"

print("With Reg.B pointed at literal cpu->B:")
seen = t._scan_v2_codegen_emits()
print(f"  fields seen: {sorted(seen.keys())}")

print()
try:
    t.test_no_unexpected_cpu_fields_in_v2_emit()
    print("UNEXPECTED PASS — lint missed the regression")
    sys.exit(1)
except AssertionError as e:
    print("CORRECT FAIL on test_no_unexpected_cpu_fields_in_v2_emit:")
    print(" ", str(e)[:300])

try:
    t.test_no_historical_deleted_fields_reappear()
    print("UNEXPECTED PASS on historical test")
    sys.exit(1)
except AssertionError as e:
    print()
    print("CORRECT FAIL on test_no_historical_deleted_fields_reappear:")
    print(" ", str(e)[:400])

print()
print("-> Lint catches a Reg.B → cpu->B regression. Score: works as designed.")
