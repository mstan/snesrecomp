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


def _stack_width_for_a(insn) -> int:
    return 1 if (getattr(insn, 'm_flag', 1) & 1) else 2


def _stack_width_for_xy(insn) -> int:
    return 1 if (getattr(insn, 'x_flag', 1) & 1) else 2


def _stack_delta_for_trampoline_scan(di: DecodedInsn) -> Optional[int]:
    """Return local cpu->S delta for stack ops.

    Positive means bytes pushed below entry S; negative means bytes
    pulled. JSR/JSL/RTS/RTL stay zero because v2 uses the host C stack
    for call/return control. TCS/TXS overwrite S, so return None to
    reset the analysis state conservatively.
    """
    insn = di.insn
    mnem = insn.mnem
    if mnem in ('PEA', 'PEI', 'PER', 'PHD'):
        return 2
    if mnem in ('PHP', 'PHB', 'PHK'):
        return 1
    if mnem == 'PHA':
        return _stack_width_for_a(insn)
    if mnem in ('PHX', 'PHY'):
        return _stack_width_for_xy(insn)
    if mnem == 'PLD':
        return -2
    if mnem in ('PLP', 'PLB'):
        return -1
    if mnem == 'PLA':
        return -_stack_width_for_a(insn)
    if mnem in ('PLX', 'PLY'):
        return -_stack_width_for_xy(insn)
    if mnem in ('TCS', 'TXS'):
        return None
    return 0


def _clamp_stack_delta(value: int) -> int:
    # Keeps malformed loops from growing an unbounded lattice while
    # preserving every realistic local stack-frame delta seen in MMX.
    return max(-64, min(64, int(value)))


def _classify_trampoline_returns(cfg: V2CFG) -> set:
    """Find returns reached with unbalanced PEI/PEA/PER pushes.

    Each propagated state is (delta, saw_immediate_push). A Return is a
    candidate only when some path reaches it with non-zero delta and at
    least one PEI/PEA/PER on that path.
    """
    if cfg.entry not in cfg.blocks:
        return set()

    in_states: Dict[DecodeKey, set] = {cfg.entry: {(0, False)}}
    worklist: List[DecodeKey] = [cfg.entry]
    flagged: set = set()

    while worklist:
        key = worklist.pop()
        block = cfg.blocks.get(key)
        if block is None:
            continue
        states = set(in_states.get(key, set()))
        if not states:
            continue

        returned = False
        for di in block.insns:
            mnem = di.insn.mnem
            if mnem in ('RTS', 'RTL', 'RTI'):
                if any(delta != 0 and saw for delta, saw in states):
                    flagged.add(di.insn.addr & 0xFFFFFF)
                returned = True
                break

            delta = _stack_delta_for_trampoline_scan(di)
            saw_push = mnem in ('PEA', 'PEI', 'PER')
            next_states = set()
            for delta0, saw in states:
                if delta is None:
                    next_states.add((0, saw or saw_push))
                else:
                    next_states.add((
                        _clamp_stack_delta(delta0 + delta),
                        saw or saw_push,
                    ))
            states = next_states

        if returned:
            continue

        for succ in block.successors:
            if succ not in cfg.blocks:
                continue
            old = in_states.get(succ, set())
            new = old | states
            if new != old:
                in_states[succ] = new
                worklist.append(succ)

    return flagged


def emit_function(rom: bytes, bank: int, start: int,
                  entry_m: int, entry_x: int,
                  *, end: Optional[int] = None,
                  func_name: Optional[str] = None,
                  dispatch_helpers=None,
                  indirect_call_tables=None,
                  indirect_dispatch=None,
                  suppressed_collector=None,
                  const_z_fold_collector=None,
                  dispatch_target_suppressed_collector=None,
                  unresolved_indirect_collector=None,
                  data_regions=None,
                  exclude_ranges: Optional[List[Tuple[int, int]]] = None,
                  tail_call_pc16: Optional[int] = None,
                  tail_call_target_name: Optional[str] = None,
                  callee_exit_mx=None,
                  callee_exit_mx_modes=None,
                  sibling_entry_pcs: Optional[set] = None,
                  hle_spc_upload=None,
                  hle_func=None,
                  hle_dispatch=None) -> str:
    """Emit a complete v2 C function source for one 65816 function.

    Pipeline:
        rom + (bank, start, entry_m, entry_x) → decode_function
                                              → build_cfg
        for each block:
            lower(insn) → IR ops
            emit_op(op)  → C lines
        block-end op → goto / fall-through wiring
    """
    base_func_name = func_name if func_name is not None else _default_func_name(bank, start)
    # HLE bypass: if cfg declared this function's PC as the SPC upload
    # entry (`hle_spc_upload <pc>` in the bank cfg), replace the entire
    # decoded body with a single RtlUploadSpcImageFromDp call. The
    # standard SNES SPC upload protocol is a length/target/data block
    # stream pointed to by a 24-bit ROM pointer in direct page; the
    # runtime walks it directly and writes into apu->ram. Game-agnostic
    # — works for any project that uses the standard protocol (verified
    # on SMW's HandleSPCUploads_Inner $00:8079 and ALttP's LoadSongBank
    # $00:8888).
    if hle_spc_upload and (start & 0xFFFF) in set(hle_spc_upload):
        variant_name = f"{base_func_name}{_variant_suffix(entry_m, entry_x)}"
        pc24 = ((bank & 0xFF) << 16) | (start & 0xFFFF)
        return "\n".join([
            f"RecompReturn {variant_name}(CpuState *cpu) {{",
            "  extern const char *g_last_recomp_func;",
            "  extern bool RtlUploadSpcImageFromDp(CpuState *cpu);",
            f"  g_last_recomp_func = \"{variant_name}\";",
            f"  RecompStackPush(\"{variant_name}\");",
            f"  cpu_dbg_funcname(\"{variant_name}\");",
            f"  cpu_trace_func_entry(cpu, 0x{pc24:06X}, \"{variant_name}\");",
            f"  cpu_trace_block(cpu, 0x{pc24:06X});",
            "  WatchdogCheck();",
            "  if (!RtlUploadSpcImageFromDp(cpu)) {",
            f"    fprintf(stderr, \"[apu] {base_func_name} HLE upload failed\\n\");",
            "  }",
            "  RecompStackPop();",
            "  return RECOMP_RETURN_NORMAL;",
            "}",
            "",
        ])

    # Generic HLE: cfg declared `hle_func <pc> <c_helper>`. Emit a
    # forwarding stub that hands control to the named C function.
    # The host runner provides the body (typically in gen_stubs.c).
    if hle_func and (start & 0xFFFF) in hle_func:
        c_helper = hle_func[start & 0xFFFF]
        variant_name = f"{base_func_name}{_variant_suffix(entry_m, entry_x)}"
        pc24 = ((bank & 0xFF) << 16) | (start & 0xFFFF)
        return "\n".join([
            f"RecompReturn {variant_name}(CpuState *cpu) {{",
            "  extern const char *g_last_recomp_func;",
            f"  extern RecompReturn {c_helper}(CpuState *cpu);",
            f"  g_last_recomp_func = \"{variant_name}\";",
            f"  RecompStackPush(\"{variant_name}\");",
            f"  cpu_dbg_funcname(\"{variant_name}\");",
            f"  cpu_trace_func_entry(cpu, 0x{pc24:06X}, \"{variant_name}\");",
            f"  cpu_trace_block(cpu, 0x{pc24:06X});",
            "  WatchdogCheck();",
            f"  RecompReturn _r = {c_helper}(cpu);",
            "  RecompStackPop();",
            "  return _r;",
            "}",
            "",
        ])

    graph = decode_function(rom, bank, start, entry_m, entry_x, end=end,
                            dispatch_helpers=dispatch_helpers,
                            indirect_call_tables=indirect_call_tables,
                            indirect_dispatch=indirect_dispatch,
                            hle_dispatch=hle_dispatch,
                            data_regions=data_regions,
                            callee_exit_mx=callee_exit_mx,
                            callee_exit_mx_modes=callee_exit_mx_modes,
                            sibling_entry_pcs=sibling_entry_pcs)
    # Forward any suppressed indirect calls upward so emit_bank can
    # aggregate them into the build report. List-of-records.
    if suppressed_collector is not None:
        suppressed_collector.extend(graph.suppressed_indirect_calls)
    # Same plumbing for the constant-Z fold log: each rewritten BEQ/BNE
    # is recorded once per (function entry, branch site).
    if const_z_fold_collector is not None:
        const_z_fold_collector.extend(graph.const_z_folds)
    # cfg-data_region dispatch-target suppressions.
    if dispatch_target_suppressed_collector is not None:
        dispatch_target_suppressed_collector.extend(
            graph.dispatch_targets_suppressed)
    # IndirectGoto / Call (abs,X) sites that couldn't be resolved (no
    # cfg directive, no auto-recovery). v2_regen hard-fails on any.
    if unresolved_indirect_collector is not None:
        unresolved_indirect_collector.extend(graph.unresolved_indirects)
    cfg = build_cfg(graph)

    # ── PEI-trampoline detector (2026-05-24, narrow variant) ──────────
    #
    # Scan the function's decoded insns for PEI / PEA / PER mnemonics.
    # Each is a 2-byte push that doesn't have a matching pop in the
    # canonical asm-balanced shape (PHP/PLP, PHA/PLA, etc.). A function
    # containing any of them is a CANDIDATE PEI-trampoline — at its
    # RTS/RTL, the topmost cpu->S bytes may be a computed return target
    # rather than the caller's pushed JSR/JSL frame.
    #
    # Flagging is per-function: every Return in a candidate function
    # gets `source_pc24 ∈ _TRAMPOLINE_RETURNS`. Codegen.py's _emit_return
    # then emits a runtime balance check (cpu->S vs _entry_s) and on
    # the unbalanced path tail-calls cpu_dispatch_pc with the popped
    # (PB:PC+1) target. The runtime check filters out balanced paths
    # through the same Return op (e.g. a join block reached from both
    # a PEI-pushing predecessor and a non-PEI predecessor).
    #
    # PHP/PLP-balanced functions don't trip the detector (no PEI/PEA/PER)
    # so the standard `return _ps;` emit is used everywhere. The Dr Light
    # case (bank_04_9A02 with 3 PEIs on path C) is caught.
    has_pei = False
    for blk in cfg.blocks.values():
        for di in blk.insns:
            mnem = di.insn.mnem
            if mnem in ('PEI', 'PEA', 'PER'):
                has_pei = True
                break
        if has_pei:
            break
    has_pei = False
    trampoline_returns_local = _classify_trampoline_returns(cfg)
    if has_pei:
        # Every Return in this function may execute on an unbalanced path
        # (we don't do path-level discrimination — the runtime check is
        # the final discriminator). Collect every RTS/RTL/RTI source_pc24
        # and add to the codegen-level set.
        for blk in cfg.blocks.values():
            last = blk.insns[-1] if blk.insns else None
            if last is None:
                continue
            m = last.insn.mnem
            if m in ('RTS', 'RTL', 'RTI'):
                trampoline_returns_local.add(last.insn.addr & 0xFFFFFF)
        from v2.codegen import add_trampoline_returns
        add_trampoline_returns(trampoline_returns_local)
    if trampoline_returns_local:
        from v2.codegen import add_trampoline_returns
        add_trampoline_returns(trampoline_returns_local)

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
    #   [<setup ops>] + [PullReg(A) × N] + (Goto | Return)
    # where N is a multiple of the return-PC byte count (2 for RTS, 3
    # for RTL). Setup ops are anything OTHER than Push/Pull/Call (those
    # would interfere with stack accounting). Three sub-cases:
    #   (a) Block ends in its own Return — single-block PLA*N RTS,
    #       e.g. a leaf "SkipCaller" function. Skip = N / unit.
    #   (b) Block ends in a Goto whose successor block ends in Return
    #       (no further stack manipulation in successor) — multi-block
    #       BNE NLRBlock; NLRBlock: PLA*N + BRA tail; tail: work + RTS.
    #   (c) Block has setup work + PLA*N + JMP cross-fn. The JMP target
    #       is decoded INTO this function's CFG (v2 inlines cross-fn
    #       branch targets), and that target eventually RTSes. The
    #       setup ops are real game-state changes — they MUST be
    #       emitted; only the PLAs and the SKIP emit are special.
    #       Yoshi-block 2026-05-02: $00:F005 has this pattern with
    #       JMP $EE35 — Mario-on-Yoshi-vs-koopa-slope death root.
    # Returns a dict {skip:int, pla_start_ir_idx:int, pla_count:int}
    # so the emit loop can preserve setup ops, skip just the PLAs, and
    # set _pending_skip before the terminator.
    import os, sys as _sys
    _NLR_DEBUG = os.environ.get('SNESRECOMP_NLR_DEBUG') == '1'
    def _detect_nlr(key: DecodeKey):
        def _dbg(msg):
            if _NLR_DEBUG:
                print(f'[nlr_dbg] {func_name} {_label_for(key)}: {msg}',
                      file=_sys.stderr, flush=True)
        ops = block_ir.get(key, [])
        if not ops:
            _dbg('REJECT: no ops')
            return None
        # Three terminator shapes:
        #   - Block ends in Return — the RTS picks up _pending_skip.
        #   - Block ends in Goto — the goto's successor must be a
        #     Return-terminated block.
        #   - Block ends in something else (e.g. PullReg, Read, Write) —
        #     the block has NO terminator IR, just an implicit
        #     fall-through to its single CFG successor. Same NLR
        #     handling as the Goto case (chase successor).
        last_op = ops[-1]
        if isinstance(last_op, (Goto, Return)):
            terminator = last_op
            scan_end_excl = len(ops) - 1
        else:
            terminator = None
            scan_end_excl = len(ops)
        # Walk backward from scan_end_excl, counting PullReg(A)s.
        i = scan_end_excl - 1
        pla_end_excl = scan_end_excl
        while i >= 0 and isinstance(ops[i], PullReg) and ops[i].reg == Reg.A:
            i -= 1
        pla_start = i + 1
        pull_count = pla_end_excl - pla_start
        _dbg(f'pull_count={pull_count} pla_start={pla_start} '
             f'terminator={type(terminator).__name__ if terminator else "fall-through"}')
        if pull_count < 2:
            return None
        # Setup region (before PLAs) must not contain Call/IndirectGoto —
        # those would interfere with stack accounting. Goto/CondBranch in
        # the middle would mean the block isn't straight-line, which isn't
        # possible at this stage of v2 (each basic block has exactly one
        # terminator), but check defensively.
        #
        # Push/Pull ops in the setup region are TOLERATED if they balance
        # against function-wide PHA/PLA accounting — see check below. This
        # handles the F971 idiom where a function does:
        #   PHA / PHA / ... / PLA STA / PLA STA / PLA PLA RTS
        # The first two PLAs are paired with the two PHAs (intra-function
        # balance); the trailing PLA PLA is the NLR skip. The old per-block
        # "no push/pull in setup" check rejected this pattern, causing the
        # recomp to emit literal pops that consumed caller-frame bytes.
        # 2026-05-21 fix for Zelda camera axis-swap bug (see ISSUES.md).
        setup_pushes = 0
        setup_pulls = 0
        for op in ops[:pla_start]:
            if isinstance(op, (Call, IndirectGoto)):
                _dbg(f'REJECT: setup region has {type(op).__name__}')
                return None
            if isinstance(op, (Goto, Return, CondBranch)):
                _dbg(f'REJECT: setup region has {type(op).__name__}')
                return None
            if isinstance(op, (PushReg, Push)):
                setup_pushes += 1
            elif isinstance(op, (PullReg, Pull)):
                setup_pulls += 1
        # Determine return-PC byte count: 2 for RTS, 3 for RTL.
        long_return = None
        if isinstance(terminator, Return):
            long_return = terminator.long
        else:
            # Goto OR implicit fall-through: chase the lone successor.
            # Must be a local block ending in Return (no further stack
            # manipulation in successor — its RTS will pick up the
            # _pending_skip we set here).
            block = cfg.blocks[key]
            succs = block.successors
            _dbg(f'no Return; successors={[_label_for(s) for s in succs]}')
            if len(succs) != 1:
                _dbg(f'REJECT: succ_count={len(succs)} (not exactly 1)')
                return None
            succ_key = succs[0]
            if succ_key not in block_ir:
                _dbg(f'REJECT: succ {_label_for(succ_key)} not in block_ir')
                return None
            succ_ops = block_ir[succ_key]
            if not succ_ops:
                _dbg(f'REJECT: succ has no ops')
                return None
            last = succ_ops[-1]
            if not isinstance(last, Return):
                _dbg(f'REJECT: succ last op = {type(last).__name__} (not Return)')
                return None
            # Successor must not contain Push/Pull (they'd interfere with
            # stack accounting). Call/IndirectGoto are tolerated — the
            # callee is balanced and doesn't affect THIS function's stack
            # frame relative to entry. Old detector pre-2026-05-02 only
            # rejected Push/Pull/Call but not IndirectGoto; my prior
            # tightening to also reject Call regressed the A3CB pattern
            # (its successor block has ALU/load/store ops only, no Call,
            # but the over-tightening in earlier draft erroneously also
            # required absence of Call which DOES match real NLR shapes
            # like the koopa-shell fix — keep loose like the original).
            for sop in succ_ops:
                if isinstance(sop, (PushReg, PullReg, Push, Pull)):
                    _dbg(f'REJECT: succ has {type(sop).__name__}')
                    return None
            long_return = last.long
        unit = 3 if long_return else 2
        if pull_count % unit != 0:
            _dbg(f'REJECT: pull_count={pull_count} not multiple of unit={unit}')
            return None
        skip = pull_count // unit
        if skip < 1 or skip > 3:
            _dbg(f'REJECT: skip={skip} out of [1,3]')
            return None
        # Function-wide PHA/PLA balance check (2026-05-21):
        # If the setup region of this block (or any other block, through
        # the trailing-PLA chase) contains PLAs that aren't paired with
        # PHAs in the same block, those PLAs MUST be paired with PHAs
        # elsewhere in the function. Otherwise the function's net stack
        # delta would consume MORE than just the NLR-skip bytes — the
        # `pull_count` we computed would not be the true "extra pops".
        #
        # Accept iff (function-wide PLAs) - (function-wide PHAs) == pull_count
        # (the only imbalance is the trailing NLR-skip pulls).
        if setup_pushes > 0 or setup_pulls > 0:
            fn_pushes = 0
            fn_pulls = 0
            for blk_ops in block_ir.values():
                for op in blk_ops:
                    if isinstance(op, (PushReg, Push)):
                        fn_pushes += 1
                    elif isinstance(op, (PullReg, Pull)):
                        fn_pulls += 1
            if fn_pulls - fn_pushes != pull_count:
                _dbg(f'REJECT: function-wide imbalance fn_pulls={fn_pulls} '
                     f'fn_pushes={fn_pushes} pull_count={pull_count}')
                return None
        _dbg(f'ACCEPT: skip={skip} pla_start={pla_start} pla_count={pull_count}')
        return {
            'skip': skip,
            'pla_start_ir_idx': pla_start,
            'pla_count': pull_count,
        }

    # ── Extended NLR detector: PLA*N at block START + branching tail ──
    #
    # Sub-case (d), 2026-05-14: PLA*N at the START of a block followed
    # by intermediate stateless logic and a conditional or
    # unconditional terminator whose every forward-reachable path
    # eventually reaches an RTS/RTL without any intervening
    # Push/Pull/Call/IndirectGoto. The PLAs eat the parent's return
    # address; the subsequent logic runs; the eventual RTS uses the
    # grandparent's return address.
    #
    # Canonical case: $00:9AEA HandleSelectionCursor.CheckMovement —
    #   PLA PLA              ; eat parent return
    #   LDA $15 / AND / LSR  ; cursor-direction logic
    #   BEQ .Return          ; branch to RTS
    #   ... cursor-move logic ...
    # .Return: RTS
    #
    # The existing detector (cases a/b/c) misses this because the
    # PLAs aren't immediately adjacent to a Return/Goto terminator —
    # they're at the start of the block, with stateless logic + a
    # CondBranch between them and the function's RTS.
    #
    # Detection is conservative: rejects if ANY reachable block has
    # a Push/Pull/Call/IndirectGoto, or if any path is dead-ended
    # (no terminator, no Return).
    def _detect_nlr_at_start(key: DecodeKey):
        def _dbg(msg):
            if _NLR_DEBUG:
                print(f'[nlr_dbg_d] {func_name} {_label_for(key)}: {msg}',
                      file=_sys.stderr, flush=True)
        ops = block_ir.get(key, [])
        if not ops:
            return None
        # Count PLA*N at the START.
        pla_end = 0
        while pla_end < len(ops) and isinstance(ops[pla_end], PullReg) \
                and ops[pla_end].reg == Reg.A:
            pla_end += 1
        pull_count = pla_end
        if pull_count < 2:
            return None
        # Determine terminator presence and trailing scan boundary.
        last_op = ops[-1]
        if isinstance(last_op, (Goto, Return, CondBranch)):
            scan_end_excl = len(ops) - 1
            terminator = last_op
        else:
            scan_end_excl = len(ops)
            terminator = None
        # Middle region (between PLAs and terminator): no stack ops, no
        # call, no nested goto/return/condbranch (each block has exactly
        # one terminator at end — defensive).
        for op in ops[pla_end:scan_end_excl]:
            if isinstance(op, (PushReg, PullReg, Push, Pull)):
                _dbg(f'REJECT: middle has {type(op).__name__}')
                return None
            if isinstance(op, (Call, IndirectGoto)):
                _dbg(f'REJECT: middle has {type(op).__name__}')
                return None
            if isinstance(op, (Goto, Return, CondBranch)):
                _dbg(f'REJECT: middle has {type(op).__name__}')
                return None
        # If the block ends in its own Return, skip = pull_count / unit
        # and we're done — but that's case (a) which the primary
        # detector already caught. Bail to avoid double-detection.
        if isinstance(terminator, Return):
            return None
        # All forward-reachable paths from this block's successors
        # must end in Return with no Push/Pull/Call along the way.
        # DFS the successor graph; memoise.
        unit_seen: List[int] = []  # collected return widths

        def _forward_clean(start_key: DecodeKey) -> bool:
            stack = [start_key]
            visited: set = set()
            while stack:
                k = stack.pop()
                if k in visited:
                    continue
                visited.add(k)
                if k not in block_ir:
                    _dbg(f'REJECT: succ {_label_for(k)} not in block_ir')
                    return False
                k_ops = block_ir[k]
                if not k_ops:
                    _dbg(f'REJECT: succ {_label_for(k)} empty')
                    return False
                k_last = k_ops[-1]
                # Scan ops up to but not including the terminator.
                if isinstance(k_last, (Goto, Return, CondBranch)):
                    scan_n = len(k_ops) - 1
                else:
                    scan_n = len(k_ops)
                for op in k_ops[:scan_n]:
                    if isinstance(op, (PushReg, PullReg, Push, Pull,
                                       Call, IndirectGoto)):
                        _dbg(f'REJECT: succ {_label_for(k)} has '
                             f'{type(op).__name__}')
                        return False
                if isinstance(k_last, Return):
                    unit_seen.append(3 if k_last.long else 2)
                    continue
                # Goto / CondBranch / fall-through: walk successors.
                k_succs = cfg.blocks[k].successors
                if not k_succs:
                    _dbg(f'REJECT: succ {_label_for(k)} no successors '
                         f'and no Return')
                    return False
                for s in k_succs:
                    stack.append(s)
            return True

        block = cfg.blocks[key]
        succs = block.successors
        if not succs:
            _dbg('REJECT: no successors and no own Return')
            return None
        for s in succs:
            if not _forward_clean(s):
                return None
        if not unit_seen:
            _dbg('REJECT: no Return reached')
            return None
        # All reached Returns must use the same width (mixing RTS and
        # RTL in one function's NLR pattern would be exotic — bail).
        if len(set(unit_seen)) != 1:
            _dbg(f'REJECT: mixed return widths {unit_seen}')
            return None
        unit = unit_seen[0]
        if pull_count % unit != 0:
            _dbg(f'REJECT: pull_count={pull_count} not multiple of '
                 f'unit={unit}')
            return None
        skip = pull_count // unit
        if skip < 1 or skip > 3:
            _dbg(f'REJECT: skip={skip} out of [1,3]')
            return None
        _dbg(f'ACCEPT (case d): skip={skip} pla_count={pull_count}')
        return {
            'skip': skip,
            'pla_start_ir_idx': 0,
            'pla_count': pull_count,
            # case-d marker: PLAs are at the START of the block, not
            # the end. The emit code uses this to pick the right insn
            # indices when the terminator is a CondBranch (not handled
            # by the case-a/b/c "has_terminator_ir" path).
            'pla_at_start': True,
        }

    # Map key -> {skip, pla_start_ir_idx, pla_count}
    nlr_skip_by_block: Dict[DecodeKey, dict] = {}
    for key in block_order:
        info = _detect_nlr(key)
        if info is None:
            info = _detect_nlr_at_start(key)
        if info is not None:
            nlr_skip_by_block[key] = info

    # Bank where THIS function's body lives. Used to compute the 24-bit
    # address of cross-function targets (which always lie within the same
    # bank — cross-BANK jumps go through Call/JSL machinery, not Goto).
    _SAME_BANK = bank & 0xFF

    def _goto_or_return(target: DecodeKey, prefix: str = "",
                         source_pc24: Optional[int] = None) -> str:
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
        # cfg `tail_call:<addr>` directive — declared sibling fall-through.
        # When the decoder's `end:` boundary cuts a routine that
        # deliberately falls into a separately-named adjacent fn (real
        # ROM idiom: two callable entry points sharing a body), cfg
        # encodes the fact via `tail_call:`. The boundary edge becomes
        # an explicit tail call to the sibling fn instead of an
        # unresolvable goto. (m, x) come from the boundary DecodeKey
        # so the right variant suffix is used.
        if (tail_call_pc16 is not None
                and tail_call_target_name is not None
                and (target.pc & 0xFFFF) == (tail_call_pc16 & 0xFFFF)):
            sib_suffix = _variant_suffix(target.m, target.x)
            # Register cross-variant Call demand so v2_regen's auto-
            # promote synthesizes the (m, x) body when it differs from
            # the sibling's cfg-declared default. Without this, the
            # C linker fails with "unresolved external _M{m}X{x}" when
            # the boundary's (m, x) doesn't match the cfg-default of
            # the sibling — exactly what tripped the zelda3 cross-
            # variant externals (NMI_RunTileMapUpdateDMA_M0X1,
            # SpotlightInternal_M1X0, etc.) on the 2026-05-17
            # tail-call-past-end class fix.
            from v2.codegen import register_call_demand
            tail_pc24 = (_SAME_BANK << 16) | (target.pc & 0xFFFF)
            register_call_demand(tail_pc24, target.m, target.x)
            return (
                f"{prefix}{{ "
                f"RecompReturn _tc = {tail_call_target_name}{sib_suffix}(cpu); "
                f"RecompStackPop(); "
                f"return _tc; "
                f"}}  /* tail_call into sibling fn at ${target.pc & 0xFFFF:04X} "
                f"(cfg tail_call: directive) */"
            )

        # Tail-call past `end:` boundary into a declared sibling
        # function. 2026-05-17 class fix for the inline-cross-fn-blocks
        # gap: zelda3 (and SMW's adjacent-entrypoint idiom) has functions
        # that fall through directly into the next sibling function with
        # no RTS — the asm is balanced end-to-end, but the C boundary the
        # cfg/ingester drew has to be honoured. Without this case the
        # decoder would either (a) inline the whole sibling body and any
        # transitive fall-throughs into the current C function (the bug
        # behind the Zelda intro-loop submodule reset 2026-05-17), or
        # (b) emit cpu_trace_unresolved_goto_trap.
        #
        # Detection: the goto target's 24-bit PC resolves to a known
        # function entry in _NAME_RESOLVER (populated by v2_regen from
        # every `func` + `name` declaration across all bank cfgs). Same-
        # bank emits a direct call; cross-bank would also work but
        # cross-bank gotos don't reach this code path (they're emitted
        # via Call/JML machinery upstream).
        #
        # Variant: tail-call to the (m, x) of the target DecodeKey — the
        # decode state at the boundary. register_call_demand records the
        # demand so v2_regen's auto-promote synthesizes the variant if
        # it isn't already in cfg.
        target_pc24 = (_SAME_BANK << 16) | (target.pc & 0xFFFF)
        src_pc24 = source_pc24 if source_pc24 is not None else 0
        from v2.codegen import get_name_for_pc, register_call_demand
        sibling_name = get_name_for_pc(target_pc24)
        if sibling_name is not None:
            sib_suffix = _variant_suffix(target.m, target.x)
            register_call_demand(target_pc24, target.m, target.x)
            return (
                f"{prefix}{{ "
                f"RecompReturn _tc = {sibling_name}{sib_suffix}(cpu); "
                f"RecompStackPop(); "
                f"return _tc; "
                f"}}  /* tail-call past end: into {sibling_name}{sib_suffix} "
                f"at ${target.pc & 0xFFFF:04X} */"
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
        # 2026-05-03 Step 2-A: emit no longer silently returns
        # RECOMP_RETURN_NORMAL. Goes through cpu_trace_unresolved_goto_trap
        # which captures (gen function name + source_pc24 + target label)
        # so a hit unambiguously identifies WHICH gen variant ran the
        # unresolvable goto — disambiguating sibling variants that
        # happen to share source PCs. The trap returns NORMAL after
        # capture (Release path) or aborts (Oracle/debug); see
        # runner/src/cpu_trace.c.
        return (
            f"{prefix}return cpu_trace_unresolved_goto_trap(cpu, "
            f"0x{src_pc24:06X}, 0x{target_pc24:06X}, "
            f"\"{func_name}\", \"{label}\");"
            f" /* {label} unresolvable cross-fn goto — "
            f"target outside this bank's import range */"
        )

    for key in block_order:
        block = cfg.blocks[key]
        lines: List[str] = []
        block_terminated = False  # True if last op was branch/goto/return/call

        # Iterate the pre-lowered (Insn, [IROp]) pairs. Calling lower()
        # again here would mint fresh Value-ids and break codegen's
        # vid → C-var mapping (GameMode00↔01 oscillation 2026-05-02).
        #
        # Pre-scan #1: identify JML-with-dispatch_entries (PHK+PER+JML or
        # PHK+PEA+JML inline-dispatch trampoline; e.g. GenerateTile_Dispatch
        # at $00:BFBC). The PHK / PEA / PER immediately preceding such a JML
        # are TRAMPOLINE SETUP that the runtime dispatcher (ExecutePtr /
        # ExecutePtrLong) would consume — under the synthesized C switch
        # we inline the dispatch directly, so those pushes would leak 3
        # garbage bytes onto the simulated SNES stack. Skip emit for them.
        # Yoshi-block freeze ROOT 2026-05-02 — see project memory.
        #
        # Pre-scan #2: NLR PLA idiom. If this block was detected as NLR
        # (PLA*N at the END of the block before a Goto/Return), figure
        # out the INSN-LEVEL indices of the PLA insns so we can skip
        # their literal-pop emit. The setup ops BEFORE the PLAs MUST
        # still emit — they're real game-state changes (e.g. STA $1DFC
        # in $00:F005's L_F024 block sets the Yoshi-knockoff sound).
        # The SKIP_N _pending_skip set is injected before the terminator
        # insn so the eventual RTS picks it up.
        pairs = block_per_insn_ir.get(key, [])
        skip_emit_idx = set()
        for ji, (jdi, _jops) in enumerate(pairs):
            if (jdi.mnem == 'JMP' and jdi.length == 4
                    and getattr(jdi, 'dispatch_entries', None)):
                k2 = ji - 1
                while k2 >= 0:
                    prev = pairs[k2][0]
                    if prev.mnem in ('PHK', 'PEA', 'PER'):
                        skip_emit_idx.add(k2)
                        k2 -= 1
                    else:
                        break

        # NLR pre-scan: map the IR-level pla_start to insn-level indices.
        # Each PLA insn lowers to exactly one PullReg(A) IR op. The
        # PLA insns are the LAST `pla_count` insns of the IR's PLA run.
        # Three terminator shapes (matching detector):
        #   - Block ends in Return-IR (RTS/RTL): last insn is RTS/RTL,
        #     PLAs are insns [last - pla_count, last).
        #   - Block ends in Goto-IR (JMP/BRA): same as above.
        #   - Block ends WITHOUT terminator IR (implicit fall-through):
        #     last insn IS one of the PLAs; PLAs are insns [last - pla_count + 1, last + 1).
        # We use the IR's `terminator` flag (None vs Goto/Return) plus
        # mnemonic spot-check.
        nlr_info = nlr_skip_by_block.get(key)
        nlr_pla_insn_indices = set()
        nlr_inject_before_idx = None  # insn index BEFORE which to inject SKIP setter
        nlr_inject_after_loop = False  # if True, emit SKIP after the per-insn loop
        if nlr_info is not None:
            pla_count = nlr_info['pla_count']
            ir_ops_flat = block_ir.get(key, [])
            if nlr_info.get('pla_at_start'):
                # Case-d: PLAs are the FIRST `pla_count` insns of the
                # block; terminator can be Goto, Return, or CondBranch
                # (any). Inject the SKIP setter BEFORE the terminator
                # insn so its eventual RTS picks it up — works for
                # CondBranch too since both legs reach a clean Return.
                pla_first_insn = 0
                pla_last_excl = pla_count
                last_ir = ir_ops_flat[-1] if ir_ops_flat else None
                has_terminator_ir = isinstance(
                    last_ir, (Goto, Return, CondBranch))
                if has_terminator_ir:
                    inject_at = len(pairs) - 1
                else:
                    inject_at = None
            else:
                has_terminator_ir = (
                    len(ir_ops_flat) > 0
                    and isinstance(ir_ops_flat[-1], (Goto, Return))
                )
                if has_terminator_ir:
                    term_insn_idx = len(pairs) - 1
                    pla_first_insn = term_insn_idx - pla_count
                    pla_last_excl = term_insn_idx
                    inject_at = term_insn_idx  # before the terminator insn
                else:
                    pla_first_insn = len(pairs) - pla_count
                    pla_last_excl = len(pairs)
                    inject_at = None  # no terminator insn → emit after loop
            if pla_first_insn < 0:
                nlr_info = None
            else:
                ok = True
                for ix in range(pla_first_insn, pla_last_excl):
                    if pairs[ix][0].mnem != 'PLA':
                        if _NLR_DEBUG:
                            print(f'[nlr_dbg] {func_name} {_label_for(key)}: '
                                  f'mnem-check FAIL at insn {ix}: '
                                  f'{pairs[ix][0].mnem!r} (expected PLA); '
                                  f'pla_first={pla_first_insn} pla_last_excl={pla_last_excl} '
                                  f'has_term={has_terminator_ir} pla_count={pla_count}',
                                  file=_sys.stderr, flush=True)
                        ok = False
                        break
                if not ok:
                    nlr_info = None
                else:
                    for ix in range(pla_first_insn, pla_last_excl):
                        nlr_pla_insn_indices.add(ix)
                        skip_emit_idx.add(ix)
                    nlr_inject_before_idx = inject_at
                    nlr_inject_after_loop = (inject_at is None)

        if _NLR_DEBUG and key in nlr_skip_by_block:
            print(f'[nlr_dbg] EMIT-LOOP {func_name} {_label_for(key)}: '
                  f'nlr_info={nlr_info!r} pla_indices={sorted(nlr_pla_insn_indices)} '
                  f'inject_before={nlr_inject_before_idx} after_loop={nlr_inject_after_loop} '
                  f'skip_emit_idx={sorted(skip_emit_idx)}',
                  file=_sys.stderr, flush=True)
        for ii, (di_insn, ir_ops) in enumerate(pairs):
            # NLR: inject _pending_skip setter + diagnostics RIGHT BEFORE
            # the terminator insn. This ensures any preceding setup ops
            # have already emitted, and the upcoming Goto/Return picks up
            # the SKIP value.
            if nlr_info is not None and ii == nlr_inject_before_idx:
                skip = nlr_info['skip']
                block_pc24 = (bank << 16) | (key.pc & 0xFFFF)
                site_label = f"{func_name}/{_label_for(key)}"
                lines.append(
                    f"cpu_trace_nlr_site_exec(cpu, 0x{block_pc24:06X}, "
                    f"\"{site_label}\");"
                )
                lines.append(
                    f"cpu_trace_event(cpu, 0, CPU_TR_NLR_DETECT, "
                    f"(uint8){skip}, 0); /* PLA*N + (Goto|RTS) = "
                    f"return-to-grandparent via SKIP_{skip} */"
                )
                lines.append(f"_pending_skip = RECOMP_RETURN_SKIP_{skip};")
                lines.append(
                    f"cpu_trace_pending_skip_write(cpu, 0x{block_pc24:06X}, "
                    f"(uint8)RECOMP_RETURN_SKIP_{skip}, \"{func_name}\");"
                )

            if ii in skip_emit_idx:
                if ii in nlr_pla_insn_indices:
                    lines.append(
                        f"/* PLA skipped — NLR idiom; SKIP_{nlr_info['skip']} "
                        f"set on _pending_skip above */"
                    )
                else:
                    lines.append(
                        f"/* trampoline setup {di_insn.mnem} skipped — "
                        f"inlined into synthesized dispatch below */"
                    )
                continue
            for op in ir_ops:
                if isinstance(op, CondBranch):
                    # Cond branch: block has TWO successors: fall-through (0)
                    # and taken-target (1) per _successors() ordering.
                    succs = block.successors
                    fall = succs[0] if len(succs) >= 1 else None
                    taken = succs[1] if len(succs) >= 2 else None
                    pred = f"{_reg_for_flag(op.flag)} == {op.take_if}"
                    blk_pc24 = (bank << 16) | (key.pc & 0xFFFF)
                    if taken is not None:
                        target_stmt = _goto_or_return(taken, source_pc24=blk_pc24)
                        lines.append(f"if ({pred}) {{ {target_stmt} }}")
                    if fall is not None:
                        lines.append(_goto_or_return(fall, source_pc24=blk_pc24)
                                     + " /* fall-through */")
                        block_terminated = True
                elif isinstance(op, Goto):
                    # JML to a registered dispatch helper (ExecutePtr /
                    # ExecutePtrLong). Bytes after the JML are a function-
                    # pointer table; the decoder already read them into
                    # insn.dispatch_entries. Synthesize a C switch that
                    # directly invokes each handler — this replaces the
                    # asm trampoline and keeps the simulated SNES stack
                    # balanced (PHK+PER setup was skipped above).
                    insn = di_insn
                    if getattr(insn, 'dispatch_entries', None):
                        from v2.codegen import _emit_dispatch
                        for ln in _emit_dispatch(insn):
                            lines.append(ln)
                        block_terminated = True
                    else:
                        succs = block.successors
                        blk_pc24 = (bank << 16) | (key.pc & 0xFFFF)
                        if len(succs) >= 1:
                            lines.append(_goto_or_return(succs[0],
                                                          source_pc24=blk_pc24))
                        else:
                            # Cross-bank JML / out-of-import-range goto.
                            # 2026-05-18 class fix: if the JML operand
                            # resolves to a registered named function in
                            # ANY bank, emit a tail-call to its
                            # M{m}X{x} variant (using the current block's
                            # (m, x) — JML doesn't touch the status
                            # register's M/X bits). This is the cross-
                            # bank analogue of the same-bank tail-call-
                            # past-end: case handled in _goto_or_return.
                            # Closes the zelda Intro_FadeInBg →
                            # Palette_FadeIntro2 JML $00:ED8F gap that
                            # was producing a `return RECOMP_RETURN_NORMAL;
                            # /* Goto with no successor */` stub (block
                            # silently bailed instead of decrementing
                            # palette_filter_countdown — title screen
                            # never advanced to attract demo).
                            target_pc24 = None
                            if (getattr(insn, 'mnem', '') == 'JMP'
                                    and getattr(insn, 'mode', None) is not None):
                                from v2.lowering import LONG as _LONG_MODE
                                if insn.mode == _LONG_MODE:
                                    target_pc24 = insn.operand & 0xFFFFFF
                            if target_pc24 is not None:
                                from v2.codegen import (get_name_for_pc,
                                                          register_call_demand)
                                tgt_name = get_name_for_pc(target_pc24)
                                if tgt_name is not None:
                                    sib_suffix = _variant_suffix(key.m, key.x)
                                    register_call_demand(target_pc24,
                                                          key.m, key.x)
                                    tgt_bank = (target_pc24 >> 16) & 0xFF
                                    # JML changes PB on real hardware
                                    # (the destination bank byte becomes
                                    # the new PB). A same-bank tail-call
                                    # past end: doesn't need this — BRA
                                    # / BRL / JMP-abs leave PB
                                    # unchanged. For cross-bank we MUST
                                    # set cpu->PB so the callee's PB-
                                    # relative addressing (PC-rel
                                    # branches, K-byte reads) computes
                                    # against the right bank. We do NOT
                                    # save/restore: this is a tail-call,
                                    # the callee's RTL returns out to
                                    # the caller's caller (just like the
                                    # asm did) — that caller's JSL
                                    # wrapper restores PB itself.
                                    lines.append(
                                        f"cpu->PB = 0x{tgt_bank:02X}; /* JML "
                                        f"into bank ${tgt_bank:02X} */"
                                    )
                                    lines.append(
                                        f"{{ RecompReturn _tc = "
                                        f"{tgt_name}{sib_suffix}(cpu); "
                                        f"RecompStackPop(); return _tc; }}"
                                        f"  /* tail-call cross-bank into "
                                        f"{tgt_name}{sib_suffix} at "
                                        f"${target_pc24:06X} (JML "
                                        f"unresolved successor) */"
                                    )
                                    block_terminated = True
                                    break  # exit `for op in ir_ops`
                            # No name resolved yet — for a CROSS-BANK
                            # JML the target is unambiguously a tail-call
                            # destination (PB changes; current function's
                            # successor block is past `end:` or in another
                            # bank). Register it as a Call demand so the
                            # next auto-promote pass synthesizes a func
                            # entry at the target. Then RE-EMIT this site
                            # as a placeholder tail-call to the synthesized
                            # `bank_BB_AAAA_MmXx` name — v2_regen will
                            # have created the entry by the next pass.
                            #
                            # This is the cross-bank tail-call class fix:
                            # we don't want to fall through to a trap and
                            # block the build on first emit; we want the
                            # demand pipeline to close the gap.
                            # In-function (same-bank) Gotos are NOT
                            # promoted (see the v2_regen.py "Goto targets
                            # are no longer auto-promoted" comment) — that
                            # would split asm routines. CROSS-bank is the
                            # safe subset: the bank change implies a
                            # function boundary.
                            src_pc24 = blk_pc24
                            tgt_str = (f"0x{target_pc24:06X}"
                                       if target_pc24 is not None else "0x000000")
                            if (target_pc24 is not None
                                    and ((target_pc24 >> 16) & 0xFF) != bank):
                                from v2.codegen import register_call_demand
                                register_call_demand(target_pc24, key.m, key.x)
                                sib_suffix = _variant_suffix(key.m, key.x)
                                tgt_bank = (target_pc24 >> 16) & 0xFF
                                tgt16 = target_pc24 & 0xFFFF
                                synth_name = (
                                    f"bank_{tgt_bank:02X}_{tgt16:04X}"
                                )
                                lines.append(
                                    f"cpu->PB = 0x{tgt_bank:02X}; /* JML "
                                    f"into bank ${tgt_bank:02X} */"
                                )
                                lines.append(
                                    f"{{ RecompReturn _tc = "
                                    f"{synth_name}{sib_suffix}(cpu); "
                                    f"RecompStackPop(); return _tc; }}"
                                    f"  /* tail-call cross-bank into "
                                    f"{synth_name}{sib_suffix} at "
                                    f"${target_pc24:06X} (auto-promoted "
                                    f"via Call demand) */"
                                )
                            else:
                                lines.append(
                                    f"return cpu_trace_unresolved_goto_trap(cpu, "
                                    f"0x{src_pc24:06X}, {tgt_str}, "
                                    f"\"{func_name}\", \"L_{key.pc & 0xFFFF:04X}_"
                                    f"M{key.m}X{key.x}\");"
                                    f"  /* unresolvable cross-bank goto — "
                                    f"no named function at target */"
                                )
                        block_terminated = True
                elif isinstance(op, Return):
                    for ln in emit_op(op):
                        lines.append(ln)
                    block_terminated = True
                elif isinstance(op, IndirectGoto):
                    # cfg-resolved (or auto-recovered) IndirectGoto: the
                    # decoder stamped `dispatch_entries` + `dispatch_idx_reg`
                    # on the insn. Route through _emit_indirect_dispatch
                    # to emit a real switch with direct tail-calls. Class
                    # fix for the IndirectGoto stub class — never emit a
                    # `/* IndirectGoto */` stub here. Unresolved sites
                    # are caught at v2_regen via graph.unresolved_indirects.
                    insn = di_insn
                    if getattr(insn, 'dispatch_entries', None):
                        from v2.codegen import _emit_indirect_dispatch
                        for ln in _emit_indirect_dispatch(insn):
                            lines.append(ln)
                        block_terminated = True
                    else:
                        # No resolution. Two sub-cases:
                        #   (a) cfg `hle_dispatch <pc16> <c_helper>` claims
                        #       this site — emit a tail-call to the named
                        #       C helper (host-side dispatcher).
                        #   (b) Otherwise emit the runtime trap so the
                        #       site is captured if execution ever reaches
                        #       it. NOT a stub (no silent fall-through);
                        #       requires HLE follow-up.
                        site_pc24 = (insn.addr & 0xFFFFFF) if insn is not None else \
                                    ((bank << 16) | (key.pc & 0xFFFF))
                        site_pc16 = site_pc24 & 0xFFFF
                        if hle_dispatch and site_pc16 in hle_dispatch:
                            c_helper = hle_dispatch[site_pc16]
                            lines.append(
                                f"{{ extern RecompReturn {c_helper}(CpuState *cpu); "
                                f"RecompReturn _r = {c_helper}(cpu); "
                                f"RecompStackPop(); return _r; }} "
                                f"/* hle_dispatch ${site_pc24:06X} — "
                                f"host-side dispatcher */"
                            )
                        else:
                            lines.append(
                                f"return cpu_trace_dispatch_oob(cpu, "
                                f"0x{site_pc24:06x}, 0xFFFF); "
                                f"/* unresolved IndirectGoto — HLE pending */")
                        block_terminated = True
                elif isinstance(op, PushReg) and getattr(
                        di_insn, 'dispatch_entries', None):
                    # RTS-stack dispatch: the decoder marked the PHA
                    # that would normally push target-1 for a following
                    # RTS. Emit a switch instead of the literal stack
                    # push, otherwise the synthesized return address
                    # leaks onto the simulated SNES stack.
                    from v2.codegen import _emit_indirect_dispatch
                    for ln in _emit_indirect_dispatch(di_insn):
                        lines.append(ln)
                    block_terminated = True
                elif isinstance(op, Call):
                    # Dispatch-helper JSL: the decoder marked the insn
                    # with `dispatch_entries`. Two routes:
                    #   (a) JSL ExecutePtr helper — index by A, switch
                    #       through `_emit_dispatch`.
                    #   (b) JSR (abs,X) authorised by `indirect_dispatch`
                    #       directive — index by X (or Y if cfg said so),
                    #       switch through `_emit_indirect_dispatch`.
                    # JSL ExecutePtr helpers are terminators. JSR
                    # (abs,X) dispatches are ordinary calls: the selected
                    # handler RTSes back to the next instruction.
                    insn = di_insn
                    if getattr(insn, 'dispatch_entries', None):
                        if getattr(insn, 'dispatch_idx_reg', None) in ('X', 'Y'):
                            from v2.codegen import _emit_indirect_dispatch
                            for ln in _emit_indirect_dispatch(insn):
                                lines.append(ln)
                            if getattr(insn, 'mnem', '') != 'JSR':
                                block_terminated = True
                        else:
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
        # NLR with no terminator IR (block IR was pure-PullReg, like
        # $01:A3CB's [PLA, PLA, fall-through]). The SKIP setter wasn't
        # injected during the per-insn loop because there was no
        # terminator insn to anchor on. Inject it here, BEFORE the
        # implicit fall-through emission below.
        if nlr_info is not None and nlr_inject_after_loop and not block_terminated:
            skip = nlr_info['skip']
            block_pc24 = (bank << 16) | (key.pc & 0xFFFF)
            site_label = f"{func_name}/{_label_for(key)}"
            lines.append(
                f"cpu_trace_nlr_site_exec(cpu, 0x{block_pc24:06X}, "
                f"\"{site_label}\");"
            )
            lines.append(
                f"cpu_trace_event(cpu, 0, CPU_TR_NLR_DETECT, "
                f"(uint8){skip}, 0); /* PLA*N + fall-through = "
                f"return-to-grandparent via SKIP_{skip} */"
            )
            lines.append(f"_pending_skip = RECOMP_RETURN_SKIP_{skip};")
            lines.append(
                f"cpu_trace_pending_skip_write(cpu, 0x{block_pc24:06X}, "
                f"(uint8)RECOMP_RETURN_SKIP_{skip}, \"{func_name}\");"
            )

        # Block didn't end with a control-flow op. Emit the explicit edge
        # to its lone CFG successor (linear fall-through) — never rely on
        # textual fall-through into whatever block_order put next, which
        # may have already been emitted earlier in DFS order. Without
        # this, e.g. L_809F's "fall through to L_80A0" silently became
        # "fall through to the function epilogue" for any block whose
        # successor was visited first.
        if not block_terminated:
            succs = block.successors
            blk_pc24 = (bank << 16) | (key.pc & 0xFFFF)
            if len(succs) == 1:
                lines.append(_goto_or_return(succs[0], source_pc24=blk_pc24)
                             + " /* implicit fall-through */")
            elif (len(succs) > 1 and pairs
                  and pairs[-1][0].mnem in ('JSR', 'JSL')):
                lines.append("switch (((cpu->m_flag & 1) << 1) | (cpu->x_flag & 1)) {")
                seen_mx = set()
                fallback_stmt = None
                for succ in succs:
                    mx = (succ.m & 1, succ.x & 1)
                    if mx in seen_mx:
                        continue
                    seen_mx.add(mx)
                    stmt = _goto_or_return(succ, source_pc24=blk_pc24)
                    if fallback_stmt is None:
                        fallback_stmt = stmt
                    idx = (mx[0] << 1) | mx[1]
                    lines.append(
                        f"  case {idx}: {stmt} /* dynamic post-call M{mx[0]}X{mx[1]} */")
                if fallback_stmt is not None:
                    lines.append(f"  default: {fallback_stmt}")
                lines.append("}")
            elif len(succs) > 1:
                lines.append(_goto_or_return(succs[0], source_pc24=blk_pc24)
                             + " /* implicit fall-through */")
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
    # Function-local NLR pending-skip — NOT cpu state. NLR-pattern blocks
    # set this before fall-through to the Return-terminated successor;
    # the Return op reads + clears it. Local-scoped so:
    #   1. NLR signaling is C control-flow state, not 65816 hardware
    #      state — no reason to keep it on CpuState.
    #   2. The optimizer can keep it in a register; no aliasing
    #      concerns through the cpu pointer.
    #   3. Different generated functions can't see each other's
    #      in-flight NLR state.
    # Preceded by a `(void)` cast so the C compiler doesn't warn when
    # NLR detection didn't fire on this function (most functions).
    src.append(f'  RecompReturn _pending_skip = RECOMP_RETURN_NORMAL;')
    src.append(f'  (void)_pending_skip;  /* unused if no NLR site in this fn */')
    # cpu->S balance marker for the PEI-trampoline detector (2026-05-24,
    # narrow variant). Captured at function entry — codegen._emit_return
    # consults it when a Return op's source_pc24 is in _TRAMPOLINE_RETURNS.
    #
    # ALWAYS emitted, not gated on this variant's local `has_pei`: the
    # trampoline-flag set is cross-variant (a Return pc24 flagged by
    # variant M0X0 also fires for M1X1's emit of the same RTS), and
    # inline-cross-fn-blocks can drag a flagged RTS into a function
    # whose own CFG has no PEI. Conditional `_entry_s` left those
    # variants with an undeclared-identifier error at the trampoline
    # branch (mmx_08_v2.c bank-08 build break, 2026-05-24).
    src.append(f'  uint16 _entry_s = cpu->S;')
    src.append(f'  (void)_entry_s;  /* used by trampoline balance check */')
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
