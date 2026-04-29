"""Control-flow graph construction for a decoded 65816 function.

Produces a CFG suitable for SSA construction (phi-node placement
via Cytron 1991). Used by ssa_placement.py.

Algorithms:
  * Block construction: find leaders (function entry, branch targets,
    instructions following branches). Group instructions by leader.
    Each block has a single entry (the leader) and a single exit (the
    terminator).
  * Predecessor/successor edges: from each block's terminator + its
    fall-through (if applicable). Handles BRA/BRL/BCC/BEQ/BNE/etc.,
    JMP ABS, RTS/RTL/RTI (no successors), JSR/JSL (fall through to
    next instruction — the call returns), unconditional terminators.
  * Iterative dominators: Cooper-Harvey-Kennedy 2006 ("A Simple, Fast
    Dominance Algorithm"). O(N²) worst case, very fast in practice
    on the small CFGs we see (typically <50 blocks per function).
  * Dominance frontiers: Cytron 1991. For each block b with multiple
    predecessors, walk each predecessor up the dominator tree until
    reaching idom(b), adding b to each visited node's DF set.

Conventions:
  * `pc` everywhere is the 16-bit program counter (insn.addr & 0xFFFF).
  * Bank-encoded full addresses are not used here — block keys are
    16-bit PCs because that's what the recompiler's emission code
    uses for label_<pc>: lookups.
  * Out-of-decoded-range branch targets are NOT successors — they're
    treated as exits (tail calls / outside-function jumps). Only
    in-decoded-range targets become CFG edges.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Dict, FrozenSet, List, Optional, Set, Tuple

from snes65816 import ABS, INDIR, INDIR_X, LONG, REL, REL16, Insn  # type: ignore


_COND_BRANCH_MNEMS = frozenset({
    'BPL', 'BMI', 'BEQ', 'BNE', 'BCC', 'BCS', 'BVS', 'BVC',
})
_UNCOND_BRANCH_MNEMS = frozenset({'BRA', 'BRL'})
_RETURN_MNEMS = frozenset({'RTS', 'RTL', 'RTI'})


# ---------------------------------------------------------------------------
# Data classes
# ---------------------------------------------------------------------------


@dataclass
class BasicBlock:
    """A maximal straight-line sequence of instructions with one entry
    (the leader) and one exit (the terminator)."""
    start_pc: int                       # leader's 16-bit PC
    insns: List[Insn] = field(default_factory=list)
    successors: List[int] = field(default_factory=list)    # block start_pcs
    predecessors: List[int] = field(default_factory=list)  # block start_pcs

    @property
    def end_pc(self) -> int:
        """One past the last byte of the block (exclusive)."""
        if not self.insns:
            return self.start_pc
        last = self.insns[-1]
        return (last.addr & 0xFFFF) + last.length


@dataclass
class CFG:
    """Control-flow graph + dominator data."""
    blocks: Dict[int, BasicBlock] = field(default_factory=dict)
    entry_pc: int = 0
    # idom[b] = immediate dominator of b (None for entry)
    idom: Dict[int, Optional[int]] = field(default_factory=dict)
    # dominators[b] = set of blocks that dominate b (includes b itself)
    dominators: Dict[int, FrozenSet[int]] = field(default_factory=dict)
    # dominance_frontier[b] = set of blocks where phis for b's defs go
    dominance_frontier: Dict[int, FrozenSet[int]] = field(default_factory=dict)


# ---------------------------------------------------------------------------
# Public entry point
# ---------------------------------------------------------------------------


def build_cfg(insns: List[Insn],
              valid_branch_targets: Set[int],
              bank: int,
              func_start: int) -> CFG:
    """Build a CFG over `insns` (already decoded for one function).

    `valid_branch_targets` is the same set the emitter uses to decide
    whether a branch operand is reachable in this function (vs out-of-
    range / tail-call). Targets not in this set don't create edges.

    `bank` and `func_start` are kept for callers that may want
    bank-encoded addresses; this module itself uses 16-bit pc only.
    """
    if not insns:
        cfg = CFG()
        return cfg

    sorted_insns = sorted(insns, key=lambda i: i.addr & 0xFFFF)
    leaders = _find_leaders(sorted_insns, valid_branch_targets)
    blocks = _build_blocks(sorted_insns, leaders)
    _link_edges(blocks, valid_branch_targets)
    entry_pc = sorted_insns[0].addr & 0xFFFF

    cfg = CFG(blocks=blocks, entry_pc=entry_pc)
    cfg.idom = _compute_idoms(cfg.blocks, entry_pc)
    cfg.dominators = _compute_dominators_from_idoms(cfg.blocks, cfg.idom, entry_pc)
    cfg.dominance_frontier = _compute_df(cfg.blocks, cfg.idom)
    return cfg


# ---------------------------------------------------------------------------
# Block construction
# ---------------------------------------------------------------------------


def _find_leaders(sorted_insns: List[Insn],
                  valid_branch_targets: Set[int]) -> Set[int]:
    """A leader starts a basic block. The first instruction is a leader.
    Every branch target (in-range) is a leader. The instruction after
    a branch / unconditional terminator is a leader (its predecessor
    block ends with the branch/terminator).
    """
    leaders: Set[int] = set()
    leaders.add(sorted_insns[0].addr & 0xFFFF)
    for target in valid_branch_targets:
        leaders.add(target & 0xFFFF)
    for i, insn in enumerate(sorted_insns):
        is_branch = (insn.mnem in _COND_BRANCH_MNEMS
                     or insn.mnem in _UNCOND_BRANCH_MNEMS
                     or insn.mnem == 'JMP'
                     or insn.mnem in _RETURN_MNEMS)
        if is_branch and i + 1 < len(sorted_insns):
            leaders.add(sorted_insns[i + 1].addr & 0xFFFF)
    return leaders


def _build_blocks(sorted_insns: List[Insn],
                  leaders: Set[int]) -> Dict[int, BasicBlock]:
    """Group instructions into blocks keyed by leader pc."""
    blocks: Dict[int, BasicBlock] = {}
    current: Optional[BasicBlock] = None
    for insn in sorted_insns:
        pc = insn.addr & 0xFFFF
        if pc in leaders:
            current = BasicBlock(start_pc=pc)
            blocks[pc] = current
        if current is not None:
            current.insns.append(insn)
    return blocks


# ---------------------------------------------------------------------------
# Edge linking
# ---------------------------------------------------------------------------


def _link_edges(blocks: Dict[int, BasicBlock],
                valid_branch_targets: Set[int]) -> None:
    """Populate successors/predecessors based on each block's terminator."""
    sorted_block_pcs = sorted(blocks)
    for i, pc in enumerate(sorted_block_pcs):
        bb = blocks[pc]
        if not bb.insns:
            continue
        terminator = bb.insns[-1]
        succs = _terminator_successors(terminator, blocks,
                                       valid_branch_targets,
                                       fallthrough_pc=(sorted_block_pcs[i + 1]
                                                       if i + 1 < len(sorted_block_pcs)
                                                       else None))
        bb.successors = succs
    for pc, bb in blocks.items():
        for s in bb.successors:
            if s in blocks:
                blocks[s].predecessors.append(pc)


def _terminator_successors(insn: Insn,
                           blocks: Dict[int, BasicBlock],
                           valid_branch_targets: Set[int],
                           fallthrough_pc: Optional[int]) -> List[int]:
    """Compute successor block start_pcs for a block ending in `insn`."""
    mn = insn.mnem
    # RTS/RTL/RTI: no successors (function exit).
    if mn in _RETURN_MNEMS:
        return []
    # JMP ABS: successor is the operand (if in-range and decoded).
    # JMP LONG / INDIR / INDIR_X: cross-function or dispatch — no
    # in-CFG successor.
    if mn == 'JMP':
        if insn.mode == ABS and insn.operand in blocks and insn.operand in valid_branch_targets:
            return [insn.operand]
        return []
    # BRA/BRL: unconditional in-bank branch.
    if mn in _UNCOND_BRANCH_MNEMS:
        target = insn.operand
        if target in blocks and target in valid_branch_targets:
            return [target]
        return []  # tail call / out-of-range
    # Conditional branch: target + fall-through.
    if mn in _COND_BRANCH_MNEMS:
        succs: List[int] = []
        target = insn.operand
        if target in blocks and target in valid_branch_targets:
            succs.append(target)
        if fallthrough_pc is not None:
            succs.append(fallthrough_pc)
        return succs
    # JSR/JSL/anything else: falls through to next instruction.
    if fallthrough_pc is not None:
        return [fallthrough_pc]
    return []


# ---------------------------------------------------------------------------
# Iterative dominators (Cooper-Harvey-Kennedy 2006)
# ---------------------------------------------------------------------------


def _compute_idoms(blocks: Dict[int, BasicBlock],
                   entry_pc: int) -> Dict[int, Optional[int]]:
    """Compute idom[b] for every block b. Returns None for entry.

    Cooper-Harvey-Kennedy iterative algorithm:
      1. Number blocks in reverse-postorder (RPO).
      2. idom[entry] = entry; all others undefined.
      3. Iterate: for each block b ≠ entry in RPO, set idom[b] to the
         intersection (via the `intersect` walk) of its already-processed
         predecessors. Repeat until no change.
    """
    rpo = _reverse_postorder(blocks, entry_pc)
    rpo_index = {pc: i for i, pc in enumerate(rpo)}
    idom: Dict[int, Optional[int]] = {pc: None for pc in blocks}
    idom[entry_pc] = entry_pc

    def intersect(b1: int, b2: int) -> int:
        finger1, finger2 = b1, b2
        while finger1 != finger2:
            while rpo_index[finger1] > rpo_index[finger2]:
                finger1 = idom[finger1] or entry_pc
            while rpo_index[finger2] > rpo_index[finger1]:
                finger2 = idom[finger2] or entry_pc
        return finger1

    changed = True
    while changed:
        changed = False
        for b in rpo:
            if b == entry_pc:
                continue
            preds = [p for p in blocks[b].predecessors
                     if idom[p] is not None]
            if not preds:
                continue
            new_idom = preds[0]
            for p in preds[1:]:
                if idom[p] is not None:
                    new_idom = intersect(new_idom, p)
            if idom[b] != new_idom:
                idom[b] = new_idom
                changed = True
    # Convention: idom[entry] = None (not entry pointing to itself).
    idom[entry_pc] = None
    return idom


def _reverse_postorder(blocks: Dict[int, BasicBlock],
                       entry_pc: int) -> List[int]:
    """RPO of the CFG starting from entry. Used by dominator iteration."""
    visited: Set[int] = set()
    post: List[int] = []

    def dfs(pc: int) -> None:
        if pc in visited or pc not in blocks:
            return
        visited.add(pc)
        for s in blocks[pc].successors:
            dfs(s)
        post.append(pc)

    dfs(entry_pc)
    return list(reversed(post))


def _compute_dominators_from_idoms(blocks: Dict[int, BasicBlock],
                                   idom: Dict[int, Optional[int]],
                                   entry_pc: int) -> Dict[int, FrozenSet[int]]:
    """Build dominators[b] = {b, idom(b), idom(idom(b)), ..., entry}."""
    dominators: Dict[int, FrozenSet[int]] = {}
    for b in blocks:
        chain: Set[int] = {b}
        cur = idom.get(b)
        while cur is not None and cur != b:
            chain.add(cur)
            nxt = idom.get(cur)
            if nxt == cur or nxt is None:
                if cur != entry_pc:
                    chain.add(entry_pc)
                break
            cur = nxt
        dominators[b] = frozenset(chain)
    return dominators


# ---------------------------------------------------------------------------
# Dominance frontier (Cytron 1991)
# ---------------------------------------------------------------------------


def _compute_df(blocks: Dict[int, BasicBlock],
                idom: Dict[int, Optional[int]]) -> Dict[int, FrozenSet[int]]:
    """For every block b, DF(b) = nodes m such that b dominates a
    predecessor of m but b does NOT strictly dominate m."""
    df: Dict[int, Set[int]] = {pc: set() for pc in blocks}
    for b, bb in blocks.items():
        if len(bb.predecessors) < 2:
            continue
        for p in bb.predecessors:
            runner = p
            # Walk up dominator tree from p until reaching idom(b).
            # Each visited node has b in its DF.
            stop = idom.get(b)
            while runner is not None and runner != stop:
                df[runner].add(b)
                nxt = idom.get(runner)
                if nxt == runner:
                    break
                runner = nxt
    return {pc: frozenset(s) for pc, s in df.items()}
