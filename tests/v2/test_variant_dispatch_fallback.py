"""Standalone validation of the variant-dispatch fallback fix.
Run: PYTHONPATH=recompiler python tests/v2/_validate_dispatch_fix.py"""
import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "recompiler"))
from v2 import codegen


def show(title, lines):
    print(f"--- {title} ---")
    for l in lines:
        print(l)
    print()


# 1) CE9A prune scenario: survivors M0X0,M1X0,M1X1 ; M0X1 pruned.
codegen.set_valid_variants({0x00CE9A: frozenset({(0, 0), (1, 0), (1, 1)})})
lines = codegen.variant_dispatch_case_lines(0x80CE9A, "bank_00_CE9A")
show("JSL $80CE9A dispatch (M0X1 pruned)", lines)
# case 1 = (m=0,x=1) must call a real variant, not no-op.
case1 = [l for l in lines if l.strip().startswith("case 1:")]
assert case1, "missing case 1"
assert "RECOMP_RETURN_NORMAL" not in case1[0], "case 1 must call a real variant"
assert "bank_00_CE9A_M0X0" in case1[0], f"expected nearest-m survivor M0X0, got: {case1[0]}"
assert not any(l.strip() == "default: _r = RECOMP_RETURN_NORMAL; break;"
               for l in lines), "default must not silently no-op"
# all 4 indices present
for idx in range(4):
    assert any(l.strip().startswith(f"case {idx}:") for l in lines), f"missing case {idx}"

# 2) Tail-call (JMP) form with pre_call.
lines2 = codegen.variant_dispatch_case_lines(
    0x80CE9A, "bank_00_CE9A", indent="        ",
    pre_call=["cpu_tailcall_inherit_return_context(_entry_s, _hrv);"])
show("indirect JMP tail-call form (M0X1 pruned)", lines2)
assert any("cpu_tailcall_inherit_return_context" in l for l in lines2)
assert any(l.strip() == "default:" for l in lines2), "default case present"
assert not any("RECOMP_RETURN_NORMAL" in l for l in lines2), "no no-op in tail form"

# 3) All-four-survive (no prune): every case calls its own variant.
codegen.set_valid_variants({})  # empty => all four
lines3 = codegen.variant_dispatch_case_lines(0x80CE9A, "bank_00_CE9A")
show("all-four-survive (pre-prune)", lines3)
assert "bank_00_CE9A_M0X1" in " ".join(lines3), "M0X1 should call itself when not pruned"

print("ALL VALIDATIONS PASSED")
