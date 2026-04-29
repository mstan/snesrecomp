"""snesrecomp.recompiler.v2.cfg

Basic-block CFG keyed by (PC, M, X). Built on top of the v2 decoder
(decoder.py) — block identity is `DecodeKey(pc, m, x)`, the same key
the decoder uses. Same `pc` reached with different `(m, x)` produces
two distinct blocks, never silently merged.

Replaces the v1 `recompiler/cfg.py` whose blocks are keyed by `pc`
alone — that was structurally unable to represent mode-divergent
joins, which is exactly what the v2 redesign is fixing.

Public API:
    build_cfg(graph: FunctionDecodeGraph) -> V2CFG

V2CFG carries:
    blocks: Dict[DecodeKey, V2Block]
    dominators: Dict[DecodeKey, DecodeKey]      # idom map
    dominance_frontier: Dict[DecodeKey, FrozenSet[DecodeKey]]
"""

from collections import defaultdict
from dataclasses import dataclass, field
from typing import Dict, FrozenSet, List, Optional, Set

import sys
import pathlib

# Allow flat-module-style imports (matches v1 test idiom and decoder.py).
_THIS_DIR = pathlib.Path(__file__).resolve().parent
_RECOMPILER_DIR = _THIS_DIR.parent
for p in (str(_THIS_DIR), str(_RECOMPILER_DIR)):
    if p not in sys.path:
        sys.path.insert(0, p)

from decoder import (  # noqa: E402
    DecodeKey, DecodedInsn, FunctionDecodeGraph,
)


# Mnemonics that END a basic block: their successors (whether 0, 1, or 2)
# start new blocks.
_BLOCK_ENDERS = frozenset({
    'RTS', 'RTL', 'RTI', 'STP', 'WAI', 'BRK',          # terminators (no succ)
    'BRA', 'BRL',                                       # unconditional branch
    'BPL', 'BMI', 'BVC', 'BVS', 'BCC', 'BCS', 'BNE', 'BEQ',  # cond branch
    'JMP', 'JSR', 'JSL',                                # control transfer
})


@dataclass
class V2Block:
    """A basic block keyed by `entry` (a DecodeKey).

    Attributes:
        entry: the DecodeKey of the first insn in this block.
        insns: ordered list of DecodedInsns in this block.
        successors: entry DecodeKeys of successor blocks. Indirect/
            cross-bank edges that aren't in the decode graph appear as
            DecodeKeys whose `pc` is outside this function — they're
            allowed in the list but won't have a corresponding block.
        predecessors: entry DecodeKeys of predecessor blocks.
    """
    entry: DecodeKey
    insns: List[DecodedInsn]
    successors: List[DecodeKey] = field(default_factory=list)
    predecessors: List[DecodeKey] = field(default_factory=list)

    @property
    def last(self) -> DecodedInsn:
        return self.insns[-1]


@dataclass
class V2CFG:
    """Control-flow graph for one function, keyed by DecodeKey.

    Attributes:
        entry: the entry DecodeKey (graph.entry).
        blocks: dict mapping each block's entry DecodeKey to its V2Block.
        dominators: idom map. dominators[entry] == entry (self-dominator
            convention). Keys missing from this dict are unreachable
            (shouldn't happen for a well-formed graph).
        dominance_frontier: per-block dominance frontier (frozenset of
            DecodeKeys). Used by SSA-phi-placement (later phase).
    """
    entry: DecodeKey
    blocks: Dict[DecodeKey, V2Block]
    dominators: Dict[DecodeKey, DecodeKey] = field(default_factory=dict)
    dominance_frontier: Dict[DecodeKey, FrozenSet[DecodeKey]] = field(default_factory=dict)


def _identify_leaders(graph: FunctionDecodeGraph,
                      preds: Dict[DecodeKey, List[DecodeKey]]) -> Set[DecodeKey]:
    """Block leaders: function entry, successors of block-ender insns,
    and join points (multiple predecessors)."""
    leaders: Set[DecodeKey] = {graph.entry}

    for key, di in graph.insns.items():
        if di.insn.mnem in _BLOCK_ENDERS:
            for s in di.successors:
                # Successor may be outside the graph (cross-bank, indirect);
                # only mark it as a leader if it's actually decoded here.
                if s in graph.insns:
                    leaders.add(s)

    for key, ps in preds.items():
        if len(ps) > 1 and key in graph.insns:
            leaders.add(key)

    return leaders


def _build_blocks(graph: FunctionDecodeGraph,
                  leaders: Set[DecodeKey]) -> Dict[DecodeKey, V2Block]:
    """For each leader, walk forward through the decoder graph (following
    the single canonical successor for non-control-flow insns) until
    reaching a block-ender or another leader. The collected insns form
    one V2Block."""
    blocks: Dict[DecodeKey, V2Block] = {}

    for leader in leaders:
        if leader not in graph.insns:
            continue
        block_insns: List[DecodedInsn] = []
        seen: Set[DecodeKey] = set()
        cur = leader
        while True:
            di = graph.insns.get(cur)
            if di is None:
                break
            if cur in seen:
                # Defensive: shouldn't happen, but guards against any
                # unexpected cycles in the per-block walk.
                break
            seen.add(cur)
            block_insns.append(di)

            if di.insn.mnem in _BLOCK_ENDERS:
                break
            # Non-control-flow insn: a single canonical fall-through successor.
            if len(di.successors) != 1:
                # Shouldn't happen for non-block-ender mnems, but be defensive.
                break
            nxt = di.successors[0]
            if nxt in leaders and nxt != leader:
                # Next belongs to another block; stop here.
                break
            cur = nxt

        if not block_insns:
            continue
        last = block_insns[-1]
        blocks[leader] = V2Block(
            entry=leader,
            insns=block_insns,
            successors=list(last.successors),
        )

    # Wire predecessors.
    for entry_key, blk in blocks.items():
        for s in blk.successors:
            if s in blocks:
                blocks[s].predecessors.append(entry_key)

    return blocks


def _compute_dominators(blocks: Dict[DecodeKey, V2Block],
                        entry: DecodeKey) -> Dict[DecodeKey, DecodeKey]:
    """Iterative Cooper-Harvey-Kennedy dominator computation.

    Convention: idom[entry] == entry. Unreachable nodes are omitted.
    """
    if entry not in blocks:
        return {}

    # Reverse-postorder traversal.
    order: List[DecodeKey] = []
    visited: Set[DecodeKey] = set()

    def dfs(node: DecodeKey):
        if node in visited:
            return
        visited.add(node)
        blk = blocks.get(node)
        if blk is None:
            return
        for s in blk.successors:
            if s in blocks:
                dfs(s)
        order.append(node)

    dfs(entry)
    rpo = list(reversed(order))
    # Map each node to its position in RPO for the intersect step.
    rpo_index = {n: i for i, n in enumerate(rpo)}

    idom: Dict[DecodeKey, DecodeKey] = {entry: entry}

    def intersect(b1: DecodeKey, b2: DecodeKey) -> DecodeKey:
        finger1, finger2 = b1, b2
        while finger1 != finger2:
            while rpo_index[finger1] > rpo_index[finger2]:
                finger1 = idom[finger1]
            while rpo_index[finger2] > rpo_index[finger1]:
                finger2 = idom[finger2]
        return finger1

    changed = True
    while changed:
        changed = False
        for node in rpo[1:]:  # skip entry
            blk = blocks[node]
            processed_preds = [p for p in blk.predecessors if p in idom]
            if not processed_preds:
                continue
            new_idom = processed_preds[0]
            for p in processed_preds[1:]:
                new_idom = intersect(p, new_idom)
            if idom.get(node) != new_idom:
                idom[node] = new_idom
                changed = True

    return idom


def _compute_dominance_frontier(
        blocks: Dict[DecodeKey, V2Block],
        idom: Dict[DecodeKey, DecodeKey]) -> Dict[DecodeKey, FrozenSet[DecodeKey]]:
    """Cytron 1991 dominance-frontier algorithm.

    For each block B with multiple predecessors, walk up the dominator
    tree from each predecessor P, adding B to DF(runner) until
    `runner == idom[B]`.
    """
    df_sets: Dict[DecodeKey, Set[DecodeKey]] = {b: set() for b in blocks}

    for node, blk in blocks.items():
        if len(blk.predecessors) < 2:
            continue
        for p in blk.predecessors:
            if p not in idom:
                continue
            runner = p
            while runner != idom.get(node) and runner in idom:
                df_sets[runner].add(node)
                next_runner = idom[runner]
                if next_runner == runner:
                    break
                runner = next_runner

    return {n: frozenset(s) for n, s in df_sets.items()}


def build_cfg(graph: FunctionDecodeGraph) -> V2CFG:
    """Build a V2CFG from a v2-decoded function graph.

    Block keys are DecodeKeys (pc, m, x). Same pc with different
    (m, x) is two distinct blocks.
    """
    # Reverse-edge map: for each key, which keys cite it as a successor?
    preds: Dict[DecodeKey, List[DecodeKey]] = defaultdict(list)
    for key, di in graph.insns.items():
        for s in di.successors:
            preds[s].append(key)

    leaders = _identify_leaders(graph, preds)
    blocks = _build_blocks(graph, leaders)
    idom = _compute_dominators(blocks, graph.entry)
    df = _compute_dominance_frontier(blocks, idom)

    return V2CFG(
        entry=graph.entry,
        blocks=blocks,
        dominators=idom,
        dominance_frontier=df,
    )
