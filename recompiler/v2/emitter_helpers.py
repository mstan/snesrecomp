"""snesrecomp.recompiler.v2.emitter_helpers

Cross-cutting emitter patterns that aren't width-bound.

Distinct from `widths.py` — this module collects code-shape helpers
that don't take an `op.width` parameter. The first inhabitant is
the JSL/JSR bank save+restore envelope.

Background — DRY_REFACTOR.md follow-up B (2026-04-30): the JSL
emitter and the dispatch-table emitter both inline the same 6-line
"save PB; trace; set PB to target bank; CALL; trace; restore PB"
sequence. The duplication has the same risk shape as the width-mask
class — a future emitter forgets one of the trace calls or the
restore, and PB drifts. Centralizing here keeps the call-site
shape in one place.
"""
from typing import List


# ── Stack push/pop micro-pattern helpers (DRY_REFACTOR follow-up C) ────
#
# The 65816 stack has post-decrement-push, pre-increment-pop semantics.
# A byte push decrements S after the write; a word push decrements S
# both before and after, so the word ends up at S+1 (low) and S+2 (high).
# Pop is the inverse.
#
# Without these helpers, every stack-touching emitter reimplements the
# decrement/increment dance. Risk class: get the order wrong (decrement
# before write in a byte push, or skip the second decrement on a word
# push) and the stack frame ends up shifted by one byte. Symptom would
# be an apparent off-by-one in subsequent PLA/PLB pulls.

def push_byte(val_expr: str) -> List[str]:
    """8-bit stack push: write at S, then decrement S."""
    return [
        f"cpu_write8(cpu, 0x00, cpu->S, {val_expr});",
        "cpu->S = (uint16)(cpu->S - 1);",
    ]


def push_word(val_expr: str) -> List[str]:
    """16-bit stack push: pre-decrement S, write 2 bytes at S, post-decrement.
    After the push, the pushed word occupies S+1 (low) and S+2 (high)."""
    return [
        "cpu->S = (uint16)(cpu->S - 1);",
        f"cpu_write16(cpu, 0x00, cpu->S, {val_expr});",
        "cpu->S = (uint16)(cpu->S - 1);",
    ]


def pop_byte_assign(target_decl: str) -> List[str]:
    """8-bit stack pop: increment S, read at S into target_decl.

    target_decl is the full LHS — e.g. `uint8 _v` (a fresh decl) or
    `cpu->A` (mutating an existing field). Caller chooses.
    """
    return [
        "cpu->S = (uint16)(cpu->S + 1);",
        f"{target_decl} = cpu_read8(cpu, 0x00, cpu->S);",
    ]


def pop_word_assign(target_decl: str) -> List[str]:
    """16-bit stack pop: increment S, read 2 bytes at S into target_decl,
    increment S again."""
    return [
        "cpu->S = (uint16)(cpu->S + 1);",
        f"{target_decl} = cpu_read16(cpu, 0x00, cpu->S);",
        "cpu->S = (uint16)(cpu->S + 1);",
    ]


# ── REP/SEP P-mirror sync envelope (DRY_REFACTOR follow-up D) ──────────
#
# REP and SEP modify the packed `cpu->P` byte. To stay correct, the
# canonical envelope is:
#   1. save old P for trace
#   2. cpu_mirrors_to_p — sync mirrors INTO P (so freshly-set _flag_Z/N
#      from a prior ALU op aren't clobbered when we modify P)
#   3. modify cpu->P (REP: AND ~mask; SEP: OR mask)
#   4. cpu_p_to_mirrors — sync P back to mirrors
#   5. cpu_trace_px_record(REP or SEP, _old_p, cpu->P)
#
# 44c96a7 fixed the regression where step 2 was missing.

# Trace-event kind constants (match cpu_trace.h):
PX_REP = 0  # /*REP*/
PX_SEP = 1  # /*SEP*/

def modify_p_via_mirrors(mask: int, kind: str) -> List[str]:
    """Emit the 5-statement REP/SEP envelope as a list of raw C
    statements (no leading indent, no enclosing braces).

    kind: "rep" → AND ~mask (clear bits); "sep" → OR mask (set bits).
    """
    if kind == "rep":
        modify = f"cpu->P = (uint8)(cpu->P & ~{mask:#04x});"
        px_kind = PX_REP
        px_label = "REP"
    elif kind == "sep":
        modify = f"cpu->P = (uint8)(cpu->P | {mask:#04x});"
        px_kind = PX_SEP
        px_label = "SEP"
    else:
        raise ValueError(f"kind must be 'rep' or 'sep', got {kind!r}")
    return [
        "uint8 _old_p = cpu->P;",
        "cpu_mirrors_to_p(cpu);",
        modify,
        "cpu_p_to_mirrors(cpu);",
        f"cpu_trace_px_record(cpu, 0, {px_kind} /*{px_label}*/, _old_p, cpu->P);",
    ]


def call_with_pb_save(target_bank: int, callee_name: str) -> List[str]:
    """Emit the 6-statement JSL bank-save/restore envelope as a list
    of raw C statements (no indentation). Caller is responsible for
    indent / brace wrapping / single-line concatenation as needed.

    Statements:
      1. uint8 _saved_pb = cpu->PB;
      2. cpu_trace_pb_change(JSL, _saved_pb -> target_bank);
      3. cpu->PB = target_bank;
      4. {callee_name}(cpu);
      5. cpu_trace_pb_change(RTL, cpu->PB -> _saved_pb);
      6. cpu->PB = _saved_pb;

    Why all 6: real 65816 hardware sets PB to the target bank for
    the call's duration, then RTL restores it. PHK inside the callee
    must push the CALLEE's bank, not the caller's — without (3) and
    (6), inner PHK/PLB sequences poison DB to bank $00. The two trace
    calls let the trace ring distinguish enter/exit transitions of
    PB; without them the trace can't reconstruct nested call chains.
    """
    return [
        "uint8 _saved_pb = cpu->PB;",
        f"cpu_trace_pb_change(cpu, 0, _saved_pb, {target_bank:#04x}, CPU_TR_JSL);",
        f"cpu->PB = {target_bank:#04x};",
        f"{callee_name}(cpu);",
        f"cpu_trace_pb_change(cpu, 0, cpu->PB, _saved_pb, CPU_TR_RTL);",
        "cpu->PB = _saved_pb;",
    ]
