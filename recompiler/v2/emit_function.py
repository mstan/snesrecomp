"""snesrecomp.recompiler.v2.emit_function

Per-function v2 emit driver: decode_function → build_cfg → IR lowering
→ codegen → C function source.

Replaces v1's `emit_function()` (recomp.py:5930+, ~2200 lines including
EmitCtx state plumbing + heuristic phi machinery). The v2 driver is
explicit and stateless: each block is lowered independently, and
control flow between blocks is wired via labels + gotos based on the
v2 CFG edges.

Public API:
    emit_function(rom, bank, start, entry_m, entry_x, *, end=None,
                  func_name=None) -> str

Returns a complete `void <func_name>(CpuState *cpu) { ... }` C source
string.
"""

import sys
import pathlib

_THIS_DIR = pathlib.Path(__file__).resolve().parent
_RECOMPILER_DIR = _THIS_DIR.parent
for p in (str(_THIS_DIR), str(_RECOMPILER_DIR)):
    if p not in sys.path:
        sys.path.insert(0, p)

from typing import Dict, List, Optional  # noqa: E402

from v2.decoder import (  # noqa: E402
    DecodeKey, DecodedInsn, FunctionDecodeGraph, decode_function, addr24,
)
from v2.cfg import V2Block, V2CFG, build_cfg  # noqa: E402
from v2.lowering import lower  # noqa: E402
from v2.codegen import emit_op  # noqa: E402
from v2.ir import (  # noqa: E402
    IROp, IRBlock, Value,
    CondBranch, Goto, IndirectGoto, Call, Return,
)


def _label_for(key: DecodeKey) -> str:
    """C label name for a block keyed by (pc, m, x)."""
    pc = key.pc & 0xFFFF
    return f"L_{pc:04X}_M{key.m}X{key.x}"


def _default_func_name(bank: int, start: int) -> str:
    return f"bank_{bank:02X}_{start:04X}"


def emit_function(rom: bytes, bank: int, start: int,
                  entry_m: int, entry_x: int,
                  *, end: Optional[int] = None,
                  func_name: Optional[str] = None) -> str:
    """Emit a complete v2 C function source for one 65816 function.

    Pipeline:
        rom + (bank, start, entry_m, entry_x) → decode_function
                                              → build_cfg
        for each block:
            lower(insn) → IR ops
            emit_op(op)  → C lines
        block-end op → goto / fall-through wiring
    """
    graph = decode_function(rom, bank, start, entry_m, entry_x, end=end)
    cfg = build_cfg(graph)

    if func_name is None:
        func_name = _default_func_name(bank, start)

    # Mint a per-function value-id counter shared across all blocks.
    counter = [0]
    def vf():
        counter[0] += 1
        return Value(vid=counter[0])

    # Lower every (insn, key) into its IR ops, in block order.
    block_lines: Dict[DecodeKey, List[str]] = {}
    block_order: List[DecodeKey] = []

    # Emit blocks in a stable order: entry first, then DFS over successors.
    visited = set()
    def order_blocks(k):
        if k in visited or k not in cfg.blocks:
            return
        visited.add(k)
        block_order.append(k)
        for s in cfg.blocks[k].successors:
            order_blocks(s)
    order_blocks(cfg.entry)

    # Any cfg block not reached via DFS (defensive — shouldn't happen for
    # a well-formed v2 graph) gets appended at the end.
    for k in cfg.blocks:
        if k not in visited:
            block_order.append(k)

    for key in block_order:
        block = cfg.blocks[key]
        lines: List[str] = []
        for di in block.insns:
            ir_ops = lower(di.insn, value_factory=vf)
            for op in ir_ops:
                if isinstance(op, CondBranch):
                    # Cond branch: block has TWO successors: fall-through (0)
                    # and taken-target (1) per _successors() ordering.
                    succs = block.successors
                    fall = succs[0] if len(succs) >= 1 else None
                    taken = succs[1] if len(succs) >= 2 else None
                    cond_emit = emit_op(op)
                    # cond_emit currently produces only a placeholder line; we
                    # rewrite it with real labels here.
                    pred = (
                        f"{_reg_for_flag(op.flag)} == {op.take_if}"
                    )
                    if taken is not None:
                        lines.append(f"if ({pred}) goto {_label_for(taken)};")
                    if fall is not None:
                        # Fall-through is implicit if the next block in
                        # block_order matches `fall`; otherwise emit goto.
                        lines.append(f"goto {_label_for(fall)}; /* fall-through */")
                elif isinstance(op, Goto):
                    succs = block.successors
                    if len(succs) >= 1:
                        lines.append(f"goto {_label_for(succs[0])};")
                    else:
                        lines.append(f"return; /* Goto with no successor */")
                elif isinstance(op, Call):
                    # Codegen produces a function-call string already.
                    for ln in emit_op(op):
                        lines.append(ln)
                elif isinstance(op, Return):
                    for ln in emit_op(op):
                        lines.append(ln)
                elif isinstance(op, IndirectGoto):
                    # Indirect — for now stub. v1 has dispatch-table-driven
                    # resolution we don't yet model.
                    for ln in emit_op(op):
                        lines.append(ln)
                    lines.append("return; /* IndirectGoto: dispatch table */")
                else:
                    for ln in emit_op(op):
                        lines.append(ln)
        block_lines[key] = lines

    # Compose the function source with labels per block.
    src: List[str] = []
    src.append(f"void {func_name}(CpuState *cpu) {{")
    for i, key in enumerate(block_order):
        src.append(f"  {_label_for(key)}:")
        for ln in block_lines[key]:
            src.append(f"    {ln}")
    # Defensive trailing return so a missing terminator doesn't fall off
    # the end of the function in the C compiler's view.
    src.append("  return;")
    src.append("}")
    return "\n".join(src) + "\n"


def _reg_for_flag(flag) -> str:
    """Helper duplicated from codegen for the local cond-branch rewrite."""
    from v2.codegen import _reg
    return _reg(flag)
