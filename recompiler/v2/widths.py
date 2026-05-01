"""snesrecomp.recompiler.v2.widths

The ONLY place width-dependent C-string literals live in the v2 codegen.

Background — DRY_REFACTOR.md (2026-04-30): four structurally-identical
width bugs shipped in eight days, each at a different `_emit_*` site
in `codegen.py`. The pattern of each fix was the same: take the
already-on-IR `op.width`, derive `0xFF`/`0xFFFF`, `0x80`/`0x8000`, etc.,
and either mask an operand or set N/Z. That per-emitter derivation is
what let the next sibling bug land — every new emitter had to remember
both the literal and the mask shape, and any one of them could forget.

Centralizing here means:
- A new emitter cannot forget to mask: it has no other way to spell it.
- A new bug shape can be fixed once, not at every site.
- The lint at `tools/lint_codegen_widths.py` mechanically rejects raw
  width literals appearing anywhere outside this module.

Functions return strings or lists of strings ready to be joined into
emitted C. They take only the IR's `width: int` (1 or 2) and raw
operand-name strings; no IR types, no decoder dependency.
"""

from typing import List


def op_mask(width: int) -> str:
    """Operand mask for a width-bound op.

    Used for any ReadReg result that feeds an ALU/shift/compare. The
    register storage is always uint16; ops with width=1 must drop
    bit 8-15 before participating in carry/borrow/sign computations.
    """
    return "0xFF" if width == 1 else "0xFFFF"


def sign_bit(width: int) -> str:
    """High bit position for N-flag derivation."""
    return "0x80" if width == 1 else "0x8000"


def carry_bit(width: int) -> str:
    """Bit position one past the high bit — for ADC/SBC carry-out
    detection (`(temp & carry_bit) != 0`)."""
    return "0x100" if width == 1 else "0x10000"


def overflow_bit(width: int) -> str:
    """V-flag bit position for BIT mem (V = bit 6 in 8-bit, bit 14 in
    16-bit). NOT to be confused with the V flag set by ADC/SBC, which
    has its own formula."""
    return "0x40" if width == 1 else "0x4000"


def ctype(width: int) -> str:
    """C type name to hold a width-bound value."""
    return "uint8" if width == 1 else "uint16"


def masked(expr: str, width: int) -> str:
    """Wrap a raw C expression in `(expr & op_mask)`. Use for any
    operand read that must be width-respecting (ReadReg result feeding
    ALU/shift/compare/bit-test).

    Concrete bug class this prevents: 8-bit shifts/ALU ops on
    `cpu->A` leaking the B-register byte (high half) into the result.
    """
    return f"({expr} & {op_mask(width)})"


def set_nz(src_expr: str, width: int) -> List[str]:
    """Emit the canonical N/Z mirror update PLUS the cpu->P refresh.

    Replaces the duplicated N/Z tail in _emit_alu / _emit_shift /
    _emit_incmem / _emit_increg / _emit_pullreg / _emit_transfer.

    Includes the `cpu->P` packed-flag update. The earlier asymmetry
    (where some emitters set mirrors but not cpu->P) was load-bearing
    only because cpu_p_to_mirrors at the next REP/SEP would re-sync —
    but that is exactly what produced the SEP/REP-clobber bug fixed
    in 44c96a7. Always updating cpu->P here keeps the packed/mirror
    pair coherent without relying on the next REP/SEP to flush it.
    """
    sign = sign_bit(width)
    return [
        f"cpu->_flag_Z = (({src_expr}) == 0) ? 1 : 0;",
        f"cpu->_flag_N = ((({src_expr}) & {sign}) != 0) ? 1 : 0;",
        "cpu->P = (uint8)((cpu->P & ~0x82) | "
        "(cpu->_flag_Z ? 0x02 : 0) | (cpu->_flag_N ? 0x80 : 0));",
    ]


def set_nz_no_p(src_expr: str, width: int) -> List[str]:
    """Same as set_nz but omits the cpu->P update.

    Some emitters intentionally update mirrors and defer the P flush
    to a containing helper that does multiple flag updates in
    sequence. Use sparingly; prefer `set_nz` (which keeps P in sync).
    """
    sign = sign_bit(width)
    return [
        f"cpu->_flag_Z = (({src_expr}) == 0) ? 1 : 0;",
        f"cpu->_flag_N = ((({src_expr}) & {sign}) != 0) ? 1 : 0;",
    ]


def set_carry_from_bit(src_expr: str, bit_mask: str) -> str:
    """Single-line C: `cpu->_flag_C = ((src & bit_mask) != 0) ? 1 : 0;`"""
    return f"cpu->_flag_C = (({src_expr}) & {bit_mask}) ? 1 : 0;"


def set_carry_from_overflow(temp_var: str, width: int,
                            polarity: str) -> str:
    """Carry derivation from a uint32 temp produced by ADD or SUB.

    polarity:
      "add" — ADC sets C when result overflows (temp & carry_bit) != 0
      "sub" — SBC sets C when result does NOT borrow, i.e. result
              fits in width: (temp & carry_bit) == 0 -> C=1
    """
    cb = carry_bit(width)
    if polarity == "add":
        return f"cpu->_flag_C = ({temp_var} & {cb}) ? 1 : 0;"
    elif polarity == "sub":
        return f"cpu->_flag_C = ({temp_var} & {cb}) ? 0 : 1;"
    raise ValueError(f"polarity must be 'add' or 'sub', got {polarity!r}")


def set_v_adc(lhs_m: str, rhs_m: str, out_v: str, width: int) -> str:
    """Two's-complement overflow flag for ADC.
    V set when sign of result differs from sign of both operands —
    i.e. (lhs ^ result) & (rhs ^ result) & sign_bit."""
    sign = sign_bit(width)
    return (f"cpu->_flag_V = ((({lhs_m} ^ {out_v}) & "
            f"({rhs_m} ^ {out_v}) & {sign}) != 0) ? 1 : 0;")


def set_v_sbc(lhs_m: str, rhs_m: str, out_v: str, width: int) -> str:
    """Two's-complement overflow flag for SBC.
    V set when sign of (lhs - rhs) overflows: (lhs ^ rhs) sign-differ
    AND (lhs ^ result) sign-differ — both into the sign bit."""
    sign = sign_bit(width)
    return (f"cpu->_flag_V = ((({lhs_m} ^ {rhs_m}) & "
            f"({lhs_m} ^ {out_v}) & {sign}) != 0) ? 1 : 0;")


# ── Register-write expression helpers ──────────────────────────────────
#
# The 65816's A/X/Y registers each have a runtime-conditional width
# semantic that can't be folded at codegen time (PLA before next REP/SEP
# can change width). The runtime `if (cpu->m_flag) { ... }` branch is
# preserved — but the inside of each branch still needs width literals.
# Centralizing here keeps the lint's no-raw-literal rule clean and makes
# the per-register hardware contract explicit in one place.

def preserve_high(field: str, lo_byte_expr: str) -> str:
    """A in m=1: keep the existing high byte (B register), replace the
    low byte. Used by WriteReg/PullReg/Transfer/IncReg.
    Returns: `(uint16)((field & 0xFF00) | (lo_byte_expr & 0xFF))`"""
    return f"(uint16)(({field} & 0xFF00) | (({lo_byte_expr}) & 0xFF))"


def zero_extend_lo(lo_byte_expr: str) -> str:
    """X/Y in x=1: hardware-zero the high byte. Used by WriteReg etc.
    Returns: `(uint16)((lo_byte_expr) & 0xFF)`"""
    return f"(uint16)(({lo_byte_expr}) & 0xFF)"


def low_byte(field: str) -> str:
    """Cast `(uint8)(field & 0xFF)` — extract low byte for an 8-bit
    op operating on a 16-bit register storage."""
    return f"(uint8)({field} & 0xFF)"
