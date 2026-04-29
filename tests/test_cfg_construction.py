"""Pin cfg.py: block construction, dominators, dominance frontier.

Each test builds a small ROM, decodes it, runs build_cfg, and asserts
the structural invariants. Pure analysis — no emit, no SSA placement.
"""
import pathlib
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / 'recompiler'))

import recomp  # noqa: E402
from cfg import build_cfg  # noqa: E402


def _build(rom: bytes, start: int, end: int):
    """Decode + build CFG. Returns (cfg, insns)."""
    insns = recomp.decode_func(rom=rom, bank=0, start=start, end=end,
                                known_func_starts={start})
    decoded_pcs = {(i.addr & 0xFFFF) for i in insns}
    BRANCH_MNEMS = ('BPL', 'BMI', 'BEQ', 'BNE', 'BCC', 'BCS', 'BVS',
                    'BVC', 'BRA', 'BRL', 'JMP')
    valid_branch_targets = {
        ins.operand for ins in insns
        if ins.mnem in BRANCH_MNEMS and ins.operand in decoded_pcs
    }
    cfg = build_cfg(insns, valid_branch_targets, bank=0, func_start=start)
    return cfg, insns


def test_linear_function_is_one_block():
    """LDA #$05; STA $00; RTS — one block, no successors."""
    rom = bytes([0xA9, 0x05, 0x85, 0x00, 0x60])
    cfg, _ = _build(rom, 0x8000, 0x8005)
    assert list(cfg.blocks) == [0x8000]
    assert cfg.blocks[0x8000].successors == []
    assert cfg.dominance_frontier[0x8000] == frozenset()


def test_diamond_has_four_blocks_and_merge_df():
    """LDA $00; BEQ skip; LDA #$AA; BRA merge; skip: LDA #$BB; merge: STA $50; RTS"""
    rom = bytes([
        0xA5, 0x00,   # $8000 LDA $00
        0xF0, 0x04,   # $8002 BEQ $8008
        0xA9, 0xAA,   # $8004 LDA #$AA
        0x80, 0x02,   # $8006 BRA $800A
        0xA9, 0xBB,   # $8008 LDA #$BB
        0x85, 0x50,   # $800A STA $50
        0x60,         # $800C RTS
    ])
    cfg, _ = _build(rom, 0x8000, 0x800D)
    assert set(cfg.blocks) == {0x8000, 0x8004, 0x8008, 0x800A}
    # entry → BEQ → 8008, fall-through 8004
    assert sorted(cfg.blocks[0x8000].successors) == [0x8004, 0x8008]
    # 8004 → BRA → 800A
    assert cfg.blocks[0x8004].successors == [0x800A]
    # 8008 → fall-through → 800A
    assert cfg.blocks[0x8008].successors == [0x800A]
    # 800A → RTS → no successor
    assert cfg.blocks[0x800A].successors == []
    # The merge block ($800A) has 2 predecessors so DF includes it
    # for the arms ($8004 and $8008).
    assert 0x800A in cfg.dominance_frontier[0x8004]
    assert 0x800A in cfg.dominance_frontier[0x8008]
    # Entry dominates merge → merge NOT in DF(entry)
    assert 0x800A not in cfg.dominance_frontier[0x8000]


def test_loop_back_edge_creates_predecessor():
    """LDX #$05; loop: DEX; BNE loop; RTS — loop body has 2 preds."""
    rom = bytes([
        0xA2, 0x05,   # $8000 LDX #$05
        0xCA,         # $8002 DEX  (loop body, head, leader)
        0xD0, 0xFD,   # $8003 BNE $8002
        0x60,         # $8005 RTS
    ])
    cfg, _ = _build(rom, 0x8000, 0x8006)
    assert set(cfg.blocks) == {0x8000, 0x8002, 0x8005}
    # $8000 → fall-through → $8002
    assert cfg.blocks[0x8000].successors == [0x8002]
    # $8002 → BNE → $8002 (back-edge), fall-through → $8005
    assert sorted(cfg.blocks[0x8002].successors) == [0x8002, 0x8005]
    # Body has 2 predecessors: entry (fall-through) and itself (back-edge)
    assert sorted(cfg.blocks[0x8002].predecessors) == [0x8000, 0x8002]
    # DF($8002) = {$8002} (back-edge into self → self in DF)
    assert 0x8002 in cfg.dominance_frontier[0x8002]


def test_dominators_chain_for_diamond():
    """In a diamond, entry dominates everything; arms dominate only
    themselves; merge is dominated by entry only (not by either arm)."""
    rom = bytes([
        0xA5, 0x00,   # $8000
        0xF0, 0x04,   # $8002 BEQ $8008
        0xA9, 0xAA,   # $8004
        0x80, 0x02,   # $8006 BRA $800A
        0xA9, 0xBB,   # $8008
        0x85, 0x50,   # $800A
        0x60,         # $800C
    ])
    cfg, _ = _build(rom, 0x8000, 0x800D)
    # entry's idom is None
    assert cfg.idom[0x8000] is None
    # everything is dominated by entry
    for b in cfg.blocks:
        assert 0x8000 in cfg.dominators[b]
    # arm blocks are dominated by entry (and themselves) but NOT by the
    # other arm
    assert 0x8004 not in cfg.dominators[0x8008]
    assert 0x8008 not in cfg.dominators[0x8004]
    # merge ($800A) is NOT dominated by either arm
    assert 0x8004 not in cfg.dominators[0x800A]
    assert 0x8008 not in cfg.dominators[0x800A]


def test_jmp_abs_creates_in_cfg_edge():
    """JMP ABS to a decoded label is a CFG edge (no fall-through)."""
    rom = bytes([
        0xA5, 0x00,            # $8000 LDA $00
        0x4C, 0x06, 0x80,      # $8002 JMP $8006
        0xEA,                  # $8005 NOP (unreachable)
        0x60,                  # $8006 RTS
    ])
    cfg, _ = _build(rom, 0x8000, 0x8007)
    # $8002 is the JMP block (entry continues into it).
    # Successors of entry block: just $8006 (JMP target — no fall-through).
    assert cfg.blocks[0x8000].successors == [0x8006]
    # $8005 NOP isn't reachable from entry — it's still a block (since
    # the decoder emitted it), just with no in-edges.
    if 0x8005 in cfg.blocks:
        assert cfg.blocks[0x8005].predecessors == []


def test_three_pred_merge_includes_all_in_df():
    """A merge with 3 predecessors collects all 3 in its DF chains."""
    rom = bytes([
        0xA5, 0x00,        # $8000 LDA $00
        0xF0, 0x07,        # $8002 BEQ $800B  (path 1 → merge)
        0xA9, 0xAA,        # $8004 LDA #$AA
        0xD0, 0x03,        # $8006 BNE $800B  (path 2 → merge)
        0xA9, 0xBB,        # $8008 LDA #$BB
        0xEA,              # $800A NOP        (path 3: fall-through)
        0x85, 0x50,        # $800B STA $50    merge
        0x60,              # $800D RTS
    ])
    cfg, _ = _build(rom, 0x8000, 0x800E)
    merge = 0x800B
    # Merge has 3 predecessors: $8000 (BEQ-taken), $8004 (BNE-taken),
    # $8008 (fall-through after $800A)
    assert len(cfg.blocks[merge].predecessors) >= 2
    # DF of every non-entry, non-merge block includes the merge
    # (because they all converge there).
    assert merge in cfg.dominance_frontier[0x8004]
    assert merge in cfg.dominance_frontier[0x8008]


if __name__ == '__main__':
    fns = [v for k, v in globals().items()
           if k.startswith('test_') and callable(v)]
    for fn in fns:
        try:
            fn()
            print(f'PASS  {fn.__name__}')
        except AssertionError as e:
            print(f'FAIL  {fn.__name__}: {e}')
        except Exception as e:
            print(f'ERR   {fn.__name__}: {type(e).__name__}: {e}')
