"""SSA phi-node placement for the recompiler.

Given a CFG (cfg.build_cfg) and per-register def-site information,
compute the set of blocks where each register needs a phi node.

Algorithm: standard Cytron et al. 1991 worklist. For each register V:
  placed = {}
  worklist = defs(V)
  while worklist:
      b = worklist.pop()
      for j in DF(b):
          if j not in placed:
              placed.add(j)         # phi for V at j
              if j not in defs(V):
                  worklist.append(j)  # j is itself a def now (the phi)
  return placed

Definitions of A/X/Y/B include both:
  * Standard 65816 instructions (LDA/LDX/INX/DEX/TAX/TYA/etc.)
  * Recompiler-synthesized defs at JSR/JSL emit time:
    - x_restores callees set self.X to a callee-known expression
      (e.g. callee ends with `LDX $15E9` before RTS, so X reaches the
      caller as `g_ram[0x15e9]`). The emit-time `self.X = x_restore`
      is a real X redef that downstream merges must phi-merge.
"""
from __future__ import annotations

from typing import Dict, FrozenSet, Iterable, Optional, Set

from snes65816 import ACC, LONG, Insn  # type: ignore
from cfg import CFG  # type: ignore


# ---------------------------------------------------------------------------
# Per-register instruction-level def classification
# ---------------------------------------------------------------------------

# A is defined by any instruction that ends with A holding a new value.
# Note: CMP/BIT/STA/PHA do NOT define A.
_A_DEFS_ALWAYS = frozenset({
    'LDA',
    'TXA', 'TYA', 'TDC', 'TSC',
    'PLA',
    'ADC', 'SBC',
    'AND', 'ORA', 'EOR',
    'XBA',     # swaps A and B → defines BOTH
})

# These define A only when targeting the accumulator (mode == ACC).
# In DP/ABS modes they're memory RMW that doesn't touch A.
_A_DEFS_ACC_ONLY = frozenset({'INC', 'DEC', 'ASL', 'LSR', 'ROL', 'ROR'})

_X_DEFS = frozenset({
    'LDX', 'TAX', 'TSX',
    'INX', 'DEX',
    'PLX',
})

_Y_DEFS = frozenset({
    'LDY', 'TAY',
    'INY', 'DEY',
    'PLY',
})

# B = high byte of 16-bit accumulator. XBA swaps it with A. In M=0
# (16-bit A) any LDA also writes the high byte. Conservative include.
_B_DEFS = frozenset({
    'XBA',
    'LDA',     # 16-bit A: high byte written
    'TDC', 'TSC',
    'PLA',     # 16-bit pull writes both halves
})


def defines_register(insn: Insn, reg: str) -> bool:
    """True if `insn` mutates `reg` ('A', 'X', 'Y', or 'B').

    Conservative over-approximation for A and B (some cases could be
    refined by checking the M flag at the def site; extra phi nodes
    are dropped by Phase 6 dead-phi elim — when that ships).
    """
    mn = insn.mnem
    if reg == 'A':
        if mn in _A_DEFS_ALWAYS:
            return True
        if mn in _A_DEFS_ACC_ONLY and insn.mode == ACC:
            return True
        return False
    if reg == 'X':
        return mn in _X_DEFS
    if reg == 'Y':
        return mn in _Y_DEFS
    if reg == 'B':
        return mn in _B_DEFS
    return False


# ---------------------------------------------------------------------------
# Compute def-site sets per register
# ---------------------------------------------------------------------------


def compute_register_defs(cfg: CFG,
                          registers: Iterable[str] = ('A', 'X', 'Y', 'B'),
                          x_restores_callees: Optional[Set[int]] = None,
                          bank: int = 0) -> Dict[str, Set[int]]:
    """For each register, return the set of block_pcs containing at
    least one def of that register.

    Function-entry implicit defs are NOT counted here (the recompiler
    handles entry values via init_a/init_x sig machinery).

    `x_restores_callees`: set of bank-encoded callee addresses
    (full_addr = (bank << 16) | local_addr) that have an x_restores
    expression in cfg. JSR/JSL to such a callee at emit time runs
    `self.X = x_restore`, redefining X. Without including these, a
    function with no LDX/INX/DEX of its own but a JSR into an
    x_restoring callee gets no X phi at downstream merges and the
    goto-arrived path's X drifts — canonical:
    HandleNormalSpriteLevelColl_019211 's koopa-shell-spawn bug.
    """
    defs: Dict[str, Set[int]] = {reg: set() for reg in registers}
    x_restores_callees = x_restores_callees or set()
    for pc, bb in cfg.blocks.items():
        for insn in bb.insns:
            for reg in registers:
                if defines_register(insn, reg):
                    defs[reg].add(pc)
            # JSR/JSL with x_restores callee = synthetic X def site
            if 'X' in registers and insn.mnem in ('JSR', 'JSL'):
                if insn.mnem == 'JSL' or insn.mode == LONG:
                    target = insn.operand
                else:
                    target = (bank << 16) | insn.operand
                if target in x_restores_callees:
                    defs['X'].add(pc)
    return defs


# ---------------------------------------------------------------------------
# Cytron worklist phi placement
# ---------------------------------------------------------------------------


def compute_phi_placements(cfg: CFG,
                           defs_per_register: Dict[str, Set[int]]
                           ) -> Dict[str, FrozenSet[int]]:
    """Return the set of block_pcs needing a phi for each register."""
    placements: Dict[str, Set[int]] = {reg: set() for reg in defs_per_register}
    for reg, def_blocks in defs_per_register.items():
        worklist = list(def_blocks)
        in_worklist: Set[int] = set(def_blocks)
        placed: Set[int] = set()
        while worklist:
            b = worklist.pop()
            in_worklist.discard(b)
            for j in cfg.dominance_frontier.get(b, frozenset()):
                if j in placed:
                    continue
                placed.add(j)
                if j not in def_blocks and j not in in_worklist:
                    worklist.append(j)
                    in_worklist.add(j)
        placements[reg] = placed
    return {reg: frozenset(s) for reg, s in placements.items()}


def phi_blocks(placements: Dict[str, FrozenSet[int]]) -> FrozenSet[int]:
    """Union of all block_pcs needing a phi for ANY register."""
    out: Set[int] = set()
    for s in placements.values():
        out |= s
    return frozenset(out)
