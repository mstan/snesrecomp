"""Missing exact M/X bodies tier down instead of borrowing a sibling."""

from v2 import codegen  # noqa: E402
from v2.codegen import emit_op  # noqa: E402
from v2.ir import Call  # noqa: E402


def test_pruned_runtime_variant_uses_abi_specific_lle_fallback():
    saved = codegen._VALID_VARIANTS
    try:
        codegen.set_valid_variants({
            0x00CE9A: frozenset({(0, 0), (1, 0), (1, 1)})})
        fallback = (
            "interp_tier_run_call_frame(cpu, 0x80ce9au, "
            "0x008000u, 3, NULL)")
        lines = codegen.variant_dispatch_case_lines(
            0x80CE9A, "bank_00_CE9A", lle_fallback=fallback)
    finally:
        codegen.set_valid_variants(saved)

    case1, = [line for line in lines if line.strip().startswith("case 1:")]
    assert fallback in case1
    assert "bank_00_CE9A_M0X0" not in case1
    assert "authoritative LLE" in case1
    default = next(
        line for line in lines if line.strip().startswith("default:"))
    assert fallback in default


def test_tail_pre_call_is_not_applied_to_lle_fallback():
    saved = codegen._VALID_VARIANTS
    try:
        codegen.set_valid_variants({0x008000: frozenset({(1, 1)})})
        lines = codegen.variant_dispatch_case_lines(
            0x008000, "Target",
            pre_call=["cpu_tailcall_inherit_return_context(_entry_s, _hrv);"],
            lle_fallback=(
                "interp_tier_dispatch_balanced(cpu, 0x008000u, "
                "0x009000u, _entry_s, _hrv)"))
    finally:
        codegen.set_valid_variants(saved)

    compiled = next(line for line in lines
                    if line.strip().startswith("case 3:"))
    lle = next(line for line in lines if line.strip().startswith("case 0:"))
    assert "cpu_tailcall_inherit_return_context" in compiled
    assert "cpu_tailcall_inherit_return_context" not in lle
    assert "interp_tier_dispatch_balanced" in lle


def test_incomplete_variant_set_requires_explicit_abi_fallback():
    saved = codegen._VALID_VARIANTS
    try:
        codegen.set_valid_variants({0x008000: frozenset({(1, 1)})})
        try:
            codegen.variant_dispatch_case_lines(0x008000, "Target")
        except ValueError as exc:
            assert "ABI-specific LLE fallback" in str(exc)
        else:
            raise AssertionError("incomplete dispatch silently lacked LLE")
    finally:
        codegen.set_valid_variants(saved)


def test_direct_jsl_missing_variant_uses_three_byte_call_frame_bridge():
    saved_variants = codegen._VALID_VARIANTS
    saved_names = dict(codegen._NAME_RESOLVER)
    try:
        codegen.set_valid_variants({0x018000: frozenset({(1, 1)})})
        codegen.set_name_resolver({0x018000: "Target"})
        source = "\n".join(emit_op(Call(
            target=0x018000, long=True, source_pc24=0x008100)))
    finally:
        codegen.set_valid_variants(saved_variants)
        codegen.set_name_resolver(saved_names)

    assert "interp_tier_run_call_frame(cpu, 0x018000u, 0x008100u, 3, NULL)" in source
    assert "case 0:" in source and "authoritative LLE" in source


def test_direct_jsr_missing_variant_uses_two_byte_call_frame_bridge():
    saved_variants = codegen._VALID_VARIANTS
    saved_names = dict(codegen._NAME_RESOLVER)
    try:
        codegen.set_valid_variants({0x008000: frozenset({(1, 1)})})
        codegen.set_name_resolver({0x008000: "Target"})
        source = "\n".join(emit_op(Call(
            target=0x008000, long=False, source_pc24=0x009100)))
    finally:
        codegen.set_valid_variants(saved_variants)
        codegen.set_name_resolver(saved_names)

    assert "interp_tier_run_call_frame(cpu, 0x008000u, 0x009100u, 2, NULL)" in source
