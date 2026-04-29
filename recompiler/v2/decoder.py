"""snesrecomp.recompiler.v2.decoder

Worklist-driven 65816 decoder keyed by (PC, M, X) entry state.

REPLACES THE V1 DECODE BUG: v1's `decode_func` (recomp.py:52-354) tracks
M/X as linear scalars and stores branch-target mode hints in
`pending_flags: Dict[PC, (m, x)]` with explicit last-writer-wins overwrite
(recomp.py:298-300 comment makes this explicit). When two predecessors
reach the same PC with different (m, x), one is silently dropped and that
PC ends up decoded with the wrong mode — which is invalid for 65816
because variable-length immediate operands (LDA #imm in M=1 vs M=0) are
2 bytes vs 3 bytes, so the dropped mode can corrupt every subsequent
instruction's PC offset.

In v2, every instruction is identified by `DecodeKey(pc, m, x)`. Two
predecessors with different mode states produce two distinct
DecodedInsn records at the same PC — both are preserved. Downstream
(v2 cfg / IR / codegen) treats them as two separate blocks.

The opcode table in `snes65816.py` and the per-instruction
`decode_insn(rom, off, pc, bank, m, x)` helper are reused as-is — they
already correctly compute variable-length immediates *given* an (m, x)
input. The bug was always in the v1 caller, not in `decode_insn`.

Public API:
    decode_function(rom, bank, start, entry_m, entry_x, *, end=None)
        -> FunctionDecodeGraph
"""

from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

import sys
import pathlib

# Allow `from snesrecomp.recompiler.v2 import ...` and standalone test imports.
_THIS_DIR = pathlib.Path(__file__).resolve().parent
_RECOMPILER_DIR = _THIS_DIR.parent
if str(_RECOMPILER_DIR) not in sys.path:
    sys.path.insert(0, str(_RECOMPILER_DIR))

from snes65816 import (  # noqa: E402
    decode_insn, lorom_offset, Insn,
    ABS, INDIR, INDIR_X, LONG,
)


def addr24(bank: int, pc: int) -> int:
    """Pack bank + 16-bit PC into a 24-bit address (matches Insn.addr)."""
    return ((bank & 0xFF) << 16) | (pc & 0xFFFF)


@dataclass(frozen=True)
class DecodeKey:
    """Identifies a decoded instruction by 24-bit address + entry M/X.

    Two DecodeKeys are equal iff (pc, m, x) all match. Same `pc` with
    different `m` or `x` is two distinct keys → two distinct decoded
    instances in the graph.
    """
    pc: int   # 24-bit ((bank << 16) | local_pc)
    m: int    # entry M flag, 0 or 1
    x: int    # entry X flag, 0 or 1


@dataclass
class DecodedInsn:
    """One instruction decoded at one specific (pc, m, x) entry state."""
    key: DecodeKey
    insn: Insn               # the underlying snes65816.Insn (m_flag/x_flag set to entry m/x)
    successors: List[DecodeKey]


@dataclass
class FunctionDecodeGraph:
    """Output of `decode_function` for one function entry.

    Attributes:
        entry: the DecodeKey we started at.
        insns: dict keyed by DecodeKey. Two entries may share `key.pc`
            iff they have different `key.m` or `key.x` — that means the
            same PC was decoded twice, once per reaching mode-state, and
            both are preserved. (This is the central correctness fix.)
    """
    entry: DecodeKey
    insns: Dict[DecodeKey, DecodedInsn] = field(default_factory=dict)

    def keys_at_pc(self, pc24: int) -> List[DecodeKey]:
        """Return all DecodeKeys with this 24-bit PC (across entry mode states)."""
        return [k for k in self.insns if k.pc == pc24]

    def insns_at_pc(self, pc24: int) -> List[DecodedInsn]:
        return [self.insns[k] for k in self.keys_at_pc(pc24)]


# Mnemonics with no fall-through successor.
_TERMINATORS = frozenset({'RTS', 'RTL', 'RTI', 'STP', 'WAI', 'BRK'})

# Mnemonics with two successors: fall-through AND taken-branch target.
_COND_BRANCHES = frozenset({'BPL', 'BMI', 'BVC', 'BVS', 'BCC', 'BCS', 'BNE', 'BEQ'})


def post_mx(insn: Insn, in_m: int, in_x: int) -> Tuple[int, int]:
    """Compute (m, x) AFTER executing `insn`, given entry (in_m, in_x).

    REP/SEP clear/set M and X bits independently per the operand bitmask.
    Other instructions don't touch M/X (XCE, PLP, RTI are unmodeled at
    this layer — they keep the entry mode; later phases may refine).
    """
    if insn.mnem == 'REP':
        m = 0 if (insn.operand & 0x20) else in_m
        x = 0 if (insn.operand & 0x10) else in_x
        return m, x
    if insn.mnem == 'SEP':
        m = 1 if (insn.operand & 0x20) else in_m
        x = 1 if (insn.operand & 0x10) else in_x
        return m, x
    return in_m, in_x


def _successors(insn: Insn, key: DecodeKey, bank: int) -> List[DecodeKey]:
    """Compute successor DecodeKeys for one decoded instruction.

    Successor mode = post-instruction (m, x) per `post_mx`. Successor PC
    depends on insn semantics:
        terminator      -> []
        BRA / BRL       -> [target]
        cond branch     -> [fall-through, target]
        JMP ABS         -> [target]
        JMP INDIR/(X)   -> []  (table-driven; caller's job)
        JMP LONG        -> []  (cross-bank; caller's job)
        JSR / JSL       -> [fall-through]  (callee opaque)
        default         -> [fall-through]
    """
    post_m, post_x = post_mx(insn, key.m, key.x)
    pc = insn.addr & 0xFFFF
    next_pc = (pc + insn.length) & 0xFFFF

    mnem = insn.mnem

    if mnem in _TERMINATORS:
        return []

    if mnem in ('BRA', 'BRL'):
        return [DecodeKey(addr24(bank, insn.operand), post_m, post_x)]

    if mnem in _COND_BRANCHES:
        return [
            DecodeKey(addr24(bank, next_pc), post_m, post_x),
            DecodeKey(addr24(bank, insn.operand), post_m, post_x),
        ]

    if mnem == 'JMP':
        if insn.mode == ABS:
            return [DecodeKey(addr24(bank, insn.operand), post_m, post_x)]
        # INDIR / INDIR_X (table-dispatch) and LONG (cross-bank) — no
        # static successors at this layer.
        return []

    # Long-jump (JML) is decoded as JMP+LONG above; JSL is its own mnem.
    if mnem in ('JSR', 'JSL'):
        return [DecodeKey(addr24(bank, next_pc), post_m, post_x)]

    # Default: linear fall-through with post-instruction mode.
    return [DecodeKey(addr24(bank, next_pc), post_m, post_x)]


def decode_function(rom: bytes, bank: int, start: int,
                    entry_m: int, entry_x: int,
                    *, end: Optional[int] = None,
                    max_insns: int = 4000) -> FunctionDecodeGraph:
    """Decode a function starting at (bank, start) with entry (m, x) state.

    Worklist over DecodeKey tuples. Each key is decoded at most once;
    same PC with divergent (m, x) produces multiple keys → multiple
    DecodedInsn records.

    Args:
        rom: the full LoROM image (bytes).
        bank: 8-bit bank number.
        start: 16-bit local PC; must be in $8000–$FFFF for LoROM mapping.
        entry_m, entry_x: entry mode state (each 0 or 1).
        end: optional exclusive end PC. Worklist items at or past `end`
            are dropped (the caller declares the next function start).
        max_insns: safety cap; raises RuntimeError if exceeded.

    Returns:
        FunctionDecodeGraph populated with all reachable (pc, m, x)
        decoding instances.

    Raises:
        ValueError on unknown opcode bytes.
        RuntimeError if max_insns exceeded.
    """
    entry_m &= 1
    entry_x &= 1
    entry_key = DecodeKey(addr24(bank, start), entry_m, entry_x)
    graph = FunctionDecodeGraph(entry=entry_key)

    worklist: List[DecodeKey] = [entry_key]

    while worklist:
        if len(graph.insns) >= max_insns:
            raise RuntimeError(
                f"v2 decoder exceeded max_insns={max_insns} at "
                f"function ${addr24(bank, start):06X}"
            )
        key = worklist.pop()
        if key in graph.insns:
            continue

        pc = key.pc & 0xFFFF
        if end is not None and pc >= end:
            continue
        if not (0x8000 <= pc <= 0xFFFF):
            # Out-of-bank reference; surface upstream by skipping here.
            continue

        try:
            offset = lorom_offset(bank, pc)
        except AssertionError:
            continue
        if offset >= len(rom):
            continue

        insn = decode_insn(rom, offset, pc, bank, m=key.m, x=key.x)
        if insn is None:
            raise ValueError(
                f"v2 decoder: unknown opcode ${rom[offset]:02X} at "
                f"${bank:02X}:{pc:04X} entry_mx=({key.m},{key.x})"
            )

        # Stamp entry mode on the Insn so downstream consumers (cfg, IR,
        # codegen) see the entry state without needing the DecodeKey.
        insn.m_flag = key.m
        insn.x_flag = key.x

        succ = _successors(insn, key, bank)
        graph.insns[key] = DecodedInsn(key=key, insn=insn, successors=succ)

        for s in succ:
            if s not in graph.insns:
                worklist.append(s)

    return graph
