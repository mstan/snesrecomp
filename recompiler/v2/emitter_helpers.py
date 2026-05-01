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
