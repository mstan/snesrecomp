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

from typing import Dict, List, Optional, Tuple  # noqa: E402

from v2.decoder import (  # noqa: E402
    DecodeKey, DecodedInsn, FunctionDecodeGraph, decode_function, addr24,
)
from v2.cfg import V2Block, V2CFG, build_cfg  # noqa: E402
from v2.lowering import lower  # noqa: E402
from v2.codegen import emit_op  # noqa: E402
from v2.ir import (  # noqa: E402
    IROp, IRBlock, Value,
    CondBranch, Goto, IndirectGoto, Call, Return,
    PullReg, PushReg, Pull, Push, Reg,
)


def _label_for(key: DecodeKey) -> str:
    """C label name for a block keyed by (pc, m, x)."""
    pc = key.pc & 0xFFFF
    return f"L_{pc:04X}_M{key.m}X{key.x}"


def _default_func_name(bank: int, start: int) -> str:
    return f"bank_{bank:02X}_{start:04X}"


def _variant_suffix(m: int, x: int) -> str:
    """Mirror of codegen._variant_suffix — kept duplicated to avoid the
    cross-module import cycle. Must stay in sync."""
    return f"_M{m & 1}X{x & 1}"


def emit_function(rom: bytes, bank: int, start: int,
                  entry_m: int, entry_x: int,
                  *, end: Optional[int] = None,
                  func_name: Optional[str] = None,
                  dispatch_helpers=None,
                  exclude_ranges: Optional[List[Tuple[int, int]]] = None) -> str:
    """Emit a complete v2 C function source for one 65816 function.

    Pipeline:
        rom + (bank, start, entry_m, entry_x) → decode_function
                                              → build_cfg
        for each block:
            lower(insn) → IR ops
            emit_op(op)  → C lines
        block-end op → goto / fall-through wiring
    """
    graph = decode_function(rom, bank, start, entry_m, entry_x, end=end,
                            dispatch_helpers=dispatch_helpers)
    cfg = build_cfg(graph)

    if func_name is None:
        func_name = _default_func_name(bank, start)
    # Always append the (m, x) variant suffix. A 65816 function reached
    # from contexts with different (m, x) is literally a different
    # instruction stream (LDA/LDX/LDY immediate widths change), so each
    # variant gets its own C body. Hand-written entry points (I_RESET,
    # I_NMI, etc.) call into the cfg-default variant via aliases that
    # emit_bank produces.
    func_name = f"{func_name}{_variant_suffix(entry_m, entry_x)}"

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

    # Set of labels that actually correspond to blocks in this function.
    # Successors that don't resolve to a local block (cross-bank, past `end:`,
    # indirect dispatch with unknown table) get tail-called as a separate
    # function. The auto-promote loop in v2_regen ensures that callable
    # function exists in the same emit pass (or a later iteration).
    local_labels = {_label_for(k) for k in block_order}

    # ── Pre-lower IR for every block ────────────────────────────────────
    # CRITICAL: lower() advances the per-function value-id counter `vf`,
    # so it must be called EXACTLY ONCE per insn. The 2026-05-02
    # GameMode-oscillation black-screen regression was a duplicate
    # lower() bug — the pre-pass and the emit loop both called lower(),
    # bumping vids out of sync with codegen's _v() mapping.
    #
    # block_per_insn_ir[key] holds (Insn, [IROp]) pairs — emit needs
    # the source Insn for Call dispatch detection.
    # block_ir[key] holds the flat list of IROps — used by the NLR
    # detector below.
    block_per_insn_ir: Dict[DecodeKey, List[Tuple[object, List[IROp]]]] = {}
    block_ir: Dict[DecodeKey, List[IROp]] = {}
    for key in block_order:
        pairs: List[Tuple[object, List[IROp]]] = []
        flat: List[IROp] = []
        for di in cfg.blocks[key].insns:
            ops = lower(di.insn, value_factory=vf)
            pairs.append((di.insn, ops))
            flat.extend(ops)
        block_per_insn_ir[key] = pairs
        block_ir[key] = flat

    # ── Non-local-return idiom detection ────────────────────────────────
    # A basic block is an NLR-block if its IR has the shape
    #   [PullReg(A) × N] + (Goto-only-terminator | Return-terminator)
    # where N is a multiple of the return-PC byte count (2 for RTS, 3
    # for RTL). Two sub-cases:
    #   (a) Block ends in its own Return — single-block PLA*N RTS,
    #       e.g. a leaf "SkipCaller" function. Skip = N / unit.
    #   (b) Block has only PullRegs and exactly one successor whose
    #       body ends in Return without further stack manipulation —
    #       multi-block "BNE NLRBlock; ... NLRBlock: PLA*N + (BRA-
    #       target or fall-through); tail: real-work + RTS".
    # In either case we set cpu->pending_skip = SKIP_N and skip the
    # literal PLA emit. The eventual Return op consumes pending_skip.
    # See RecompReturn enum in cpu_state.h.
    def _detect_nlr(key: DecodeKey) -> int:
        ops = block_ir.get(key, [])
        if not ops:
            return 0
        pull_count = 0
        rest_start = 0
        for i, op in enumerate(ops):
            if isinstance(op, PullReg) and op.reg == Reg.A:
                pull_count += 1
            else:
                rest_start = i
                break
        else:
            rest_start = len(ops)
        if pull_count < 2:
            return 0
        rest = ops[rest_start:]
        long_return = None
        return_ops = [op for op in rest if isinstance(op, Return)]
        if return_ops:
            # Sub-case (a): rest contains a Return. Reject if any other
            # state-touching op is in `rest` (Goto / CondBranch tolerated
            # only as control-flow, not as work).
            for op in rest:
                if isinstance(op, (PushReg, PullReg, Push, Pull)):
                    return 0
                if isinstance(op, (Call, IndirectGoto)):
                    return 0
            long_return = return_ops[-1].long
        else:
            # Sub-case (b): no Return in rest; only Goto allowed.
            for op in rest:
                if isinstance(op, Goto):
                    continue
                return 0
            block = cfg.blocks[key]
            succs = block.successors
            if len(succs) != 1:
                return 0
            succ_key = succs[0]
            if succ_key not in block_ir:
                return 0
            succ_ops = block_ir[succ_key]
            if not succ_ops:
                return 0
            last = succ_ops[-1]
            if not isinstance(last, Return):
                return 0
            for sop in succ_ops:
                if isinstance(sop, (PushReg, PullReg, Push, Pull)):
                    return 0
            long_return = last.long
        unit = 3 if long_return else 2
        if pull_count % unit != 0:
            return 0
        skip = pull_count // unit
        if skip < 1 or skip > 3:
            return 0
        return skip

    nlr_skip_by_block: Dict[DecodeKey, int] = {}
    for key in block_order:
        s = _detect_nlr(key)
        if s > 0:
            nlr_skip_by_block[key] = s

    # Bank where THIS function's body lives. Used to compute the 24-bit
    # address of cross-function targets (which always lie within the same
    # bank — cross-BANK jumps go through Call/JSL machinery, not Goto).
    _SAME_BANK = bank & 0xFF

    def _goto_or_return(target: DecodeKey, prefix: str = "") -> str:
        label = _label_for(target)
        if label in local_labels:
            return f"{prefix}goto {label};"
        # HLE-replacement check: cfg `exclude_range S E` carves out a
        # data region — by convention, asm whose lifted form is replaced
        # by host-side HLE (e.g. the SMW asm main loop at $00:806B-$8078,
        # which the runner replaces with SmwRunOneFrameOfGame).
        # A cross-fn jump TARGETING such a range can never reach asm
        # code: there isn't any. The runner owns control-flow at that
        # address. The right shape is `return to host` — let the
        # outer-frame's HLE mechanism take over.
        # Skip auto-promote recording so the target doesn't get
        # synthesized as an empty BankEntry next pass.
        if exclude_ranges:
            tpc = target.pc & 0xFFFF
            for (lo, hi) in exclude_ranges:
                if lo <= tpc < hi:
                    return (
                        f"{prefix}return RECOMP_RETURN_NORMAL; "
                        f"/* {label} HLE-replaced "
                        f"(cfg exclude_range {lo:04X}-{hi:04X}) */"
                    )
        # Unresolvable cross-function jump.
        #
        # With the inline-cross-fn-blocks model (2026-05-02), the decoder
        # imports BRA/BRL/JMP-ABS/cond-branch targets that lie past the
        # cfg `end:` boundary directly into THIS function's CFG, so the
        # vast majority of intra-bank cross-fn jumps resolve as local
        # labels above. Anything that reaches HERE is one of:
        #   - a cross-BANK jump (JML/long-JMP — the decoder doesn't decode
        #     other banks; out of scope for in-bank inlining),
        #   - a target outside [0x8000, 0xFFFF] (data/header region),
        #   - a pathological cfg setup we couldn't import.
        #
        # We do NOT auto-promote the target into a separate C function:
        # that was the prior policy and it stranded PHB/PLB pairs across
        # C scopes (caused DB=$C0 at dispatch entry — the title-screen
        # regression). Auto-promote only synthesizes for true subroutine
        # entries (JSR/JSL targets), not for arbitrary jump destinations.
        #
        # Emit a LOUD return so the fallback is observable both at
        # gen-source review (clear comment) and in any future lint pass
        # that scans for `unresolvable cross-fn`. Auto-promote is NOT
        # invoked.
        return (
            f"{prefix}return RECOMP_RETURN_NORMAL; "
            f"/* {label} unresolvable cross-fn goto — "
            f"target outside this bank's import range */"
        )

    for key in block_order:
        block = cfg.blocks[key]
        lines: List[str] = []
        block_terminated = False  # True if last op was branch/goto/return/call

        # NLR shortcut: if this block matches the PLA*N+RTS idiom
        # (single-block) or PLA*N+fall-through-to-RTS-block (multi-
        # block), set pending_skip and either emit the block's own
        # Return inline (sub-case a) or fall through to the lone
        # Return-terminated successor (sub-case b). Skip the literal
        # PLAs — they would consume ancestor stack data since v2
        # doesn't push return-PC bytes onto the simulated SNES stack.
        if key in nlr_skip_by_block:
            skip = nlr_skip_by_block[key]
            ops_here = block_ir.get(key, [])
            block_pc24 = (bank << 16) | (key.pc & 0xFFFF)
            site_label = f"{func_name}/{_label_for(key)}"
            # Non-rotating site-exec counter (survives ring rotation).
            lines.append(
                f"cpu_trace_nlr_site_exec(cpu, 0x{block_pc24:06X}, "
                f"\"{site_label}\");"
            )
            lines.append(
                f"cpu_trace_event(cpu, 0, CPU_TR_NLR_DETECT, "
                f"(uint8){skip}, 0); /* PLA*N + RTS = "
                f"return-to-grandparent via SKIP_{skip} */"
            )
            lines.append(f"cpu->pending_skip = (uint8)RECOMP_RETURN_SKIP_{skip};")
            # Non-rotating pending_skip-write counter + first-writer
            # forensics (frame, PC, function name).
            lines.append(
                f"cpu_trace_pending_skip_write(cpu, 0x{block_pc24:06X}, "
                f"(uint8)RECOMP_RETURN_SKIP_{skip}, \"{func_name}\");"
            )
            if ops_here and isinstance(ops_here[-1], Return):
                # Sub-case (a): block ends in its own Return — emit it
                # inline so pending_skip is consumed immediately.
                for ln in emit_op(ops_here[-1]):
                    lines.append(ln)
            else:
                # Sub-case (b): fall through to the Return-terminated
                # successor whose RTS will consume pending_skip.
                succs = block.successors
                if len(succs) >= 1:
                    lines.append(_goto_or_return(succs[0]) + " /* NLR fall-through */")
                else:
                    # Defensive — _detect_nlr requires exactly one successor.
                    lines.append("return RECOMP_RETURN_NORMAL; /* NLR but no succ */")
            block_terminated = True
            block_lines[key] = lines
            continue

        # Iterate the pre-lowered (Insn, [IROp]) pairs. Calling lower()
        # again here would mint fresh Value-ids and break codegen's
        # vid → C-var mapping (GameMode00↔01 oscillation 2026-05-02).
        for (di_insn, ir_ops) in block_per_insn_ir.get(key, []):
            for op in ir_ops:
                if isinstance(op, CondBranch):
                    # Cond branch: block has TWO successors: fall-through (0)
                    # and taken-target (1) per _successors() ordering.
                    succs = block.successors
                    fall = succs[0] if len(succs) >= 1 else None
                    taken = succs[1] if len(succs) >= 2 else None
                    pred = f"{_reg_for_flag(op.flag)} == {op.take_if}"
                    if taken is not None:
                        target_stmt = _goto_or_return(taken)
                        lines.append(f"if ({pred}) {{ {target_stmt} }}")
                    if fall is not None:
                        lines.append(_goto_or_return(fall) + " /* fall-through */")
                        block_terminated = True
                elif isinstance(op, Goto):
                    succs = block.successors
                    if len(succs) >= 1:
                        lines.append(_goto_or_return(succs[0]))
                    else:
                        lines.append(f"return RECOMP_RETURN_NORMAL; /* Goto with no successor */")
                    block_terminated = True
                elif isinstance(op, Return):
                    for ln in emit_op(op):
                        lines.append(ln)
                    block_terminated = True
                elif isinstance(op, IndirectGoto):
                    for ln in emit_op(op):
                        lines.append(ln)
                    lines.append("return RECOMP_RETURN_NORMAL; /* IndirectGoto: dispatch table */")
                    block_terminated = True
                elif isinstance(op, Call):
                    # Dispatch-helper JSL: the decoder marked the insn
                    # with `dispatch_entries`. Route through _emit_dispatch
                    # — the helper itself never returns to the JSL caller;
                    # it returns to the dispatched handler. So this is
                    # a TERMINATOR.
                    insn = di_insn
                    if getattr(insn, 'dispatch_entries', None):
                        from v2.codegen import _emit_dispatch
                        for ln in _emit_dispatch(insn):
                            lines.append(ln)
                        block_terminated = True
                    else:
                        for ln in emit_op(op):
                            lines.append(ln)
                else:
                    # ReadReg, ALU, Read/Write, etc. — non-terminating.
                    for ln in emit_op(op):
                        lines.append(ln)
        # Block didn't end with a control-flow op. Emit the explicit edge
        # to its lone CFG successor (linear fall-through) — never rely on
        # textual fall-through into whatever block_order put next, which
        # may have already been emitted earlier in DFS order. Without
        # this, e.g. L_809F's "fall through to L_80A0" silently became
        # "fall through to the function epilogue" for any block whose
        # successor was visited first.
        if not block_terminated:
            succs = block.successors
            if len(succs) >= 1:
                lines.append(_goto_or_return(succs[0]) + " /* implicit fall-through */")
            else:
                lines.append("return RECOMP_RETURN_NORMAL; /* no terminator, no successor */")
        block_lines[key] = lines

    # Compose the function source with labels per block.
    src: List[str] = []
    src.append(f"RecompReturn {func_name}(CpuState *cpu) {{")
    # Diagnostics — same call-stack plumbing v1 emitted, so the runtime
    # debug_server's `call_stack` cmd and crash-handler attribution work.
    src.append(f'  extern const char *g_last_recomp_func;')
    src.append(f'  g_last_recomp_func = "{func_name}";')
    src.append(f'  RecompStackPush("{func_name}");')
    src.append(f'  cpu_dbg_funcname("{func_name}");')
    # Trace ring: function entry (carries name hash) — first entry per call.
    fn_entry_pc = (bank << 16) | (start & 0xFFFF)
    src.append(f'  cpu_trace_func_entry(cpu, 0x{fn_entry_pc:06X}, "{func_name}");')
    for i, key in enumerate(block_order):
        src.append(f"  {_label_for(key)}:")
        # Trace block entry — gives us the SNES PC chain in the trace ring.
        block_pc24 = (bank << 16) | (key.pc & 0xFFFF)
        src.append(f'    cpu_trace_block(cpu, 0x{block_pc24:06X});')
        # Watchdog: per-block heartbeat so tight inner loops trip the 5s
        # frame timeout instead of freezing the runtime indefinitely.
        # Cheap (counter bump + branch); v1 emitted at loop headers, v2
        # gets it at every block since we don't yet identify back-edges.
        src.append(f'    WatchdogCheck();')
        for ln in block_lines[key]:
            # Inject RecompStackPop before any return so the stack stays balanced.
            stripped = ln.strip()
            if stripped.startswith("return"):
                src.append(f"    RecompStackPop();")
            src.append(f"    {ln}")
    # Defensive trailing return so a missing terminator doesn't fall off
    # the end of the function in the C compiler's view.
    src.append("  RecompStackPop();")
    src.append("  return RECOMP_RETURN_NORMAL;")
    src.append("}")
    return "\n".join(src) + "\n"


def _reg_for_flag(flag) -> str:
    """Helper duplicated from codegen for the local cond-branch rewrite."""
    from v2.codegen import _reg
    return _reg(flag)
