"""Pin v2 cfg: same `pc` reached with different (m, x) produces two
distinct blocks. Dominator tree reflects the mode split."""
from _helpers import make_lorom_bank0  # noqa: E402

from v2.decoder import decode_function, DecodeKey  # noqa: E402
from v2.cfg import build_cfg, V2Block  # noqa: E402


def test_same_pc_different_mx_two_blocks():
    """
    $8000 B0 0A    BCS $800C       ; -> $800C with (m=1, x=1)
    $8002 C2 30    REP #$30        ; (m=0, x=0)
    $8004 80 06    BRA $800C       ; -> $800C with (m=0, x=0)
    $800C EA       NOP             ; reached with two different (m, x)
    $800D 60       RTS

    Block leaders include $800C twice — once at (m=1,x=1), once at
    (m=0,x=0). Two distinct V2Blocks; neither dominates the other.
    """
    rom = make_lorom_bank0({
        0x8000: bytes([0xB0, 0x0A, 0xC2, 0x30, 0x80, 0x06]),
        0x800C: bytes([0xEA, 0x60]),
    })
    graph = decode_function(rom, bank=0, start=0x8000, entry_m=1, entry_x=1)
    cfg = build_cfg(graph)

    blocks_at_800c = [k for k in cfg.blocks if (k.pc & 0xFFFF) == 0x800C]
    assert len(blocks_at_800c) == 2, (
        f"expected 2 V2Blocks at $800C (one per reaching mode); got "
        f"{len(blocks_at_800c)}: {blocks_at_800c}"
    )
    mx_set = {(k.m, k.x) for k in blocks_at_800c}
    assert mx_set == {(1, 1), (0, 0)}, f"unexpected mode pairs: {mx_set}"

    # Each $800C-block should have NOP+RTS as its insns and end with RTS.
    for k in blocks_at_800c:
        blk = cfg.blocks[k]
        mnems = [di.insn.mnem for di in blk.insns]
        assert mnems == ['NOP', 'RTS'], f"block at {k} has insns {mnems}"


def test_dominator_tree_handles_mode_split():
    """In the layout above, neither $800C-block dominates the other, but
    both are dominated by the single block at $8000."""
    rom = make_lorom_bank0({
        0x8000: bytes([0xB0, 0x0A, 0xC2, 0x30, 0x80, 0x06]),
        0x800C: bytes([0xEA, 0x60]),
    })
    graph = decode_function(rom, bank=0, start=0x8000, entry_m=1, entry_x=1)
    cfg = build_cfg(graph)

    entry_key = graph.entry
    assert cfg.dominators[entry_key] == entry_key, "entry dominates itself"

    # Both $800C blocks should have a non-self idom that traces back to entry.
    blocks_at_800c = [k for k in cfg.blocks if (k.pc & 0xFFFF) == 0x800C]
    for k in blocks_at_800c:
        assert k in cfg.dominators, f"missing idom for {k}"
        # idom is some predecessor; transitive close eventually reaches entry.
        seen = set()
        cur = k
        while cfg.dominators[cur] != cur and cur not in seen:
            seen.add(cur)
            cur = cfg.dominators[cur]
        assert cur == entry_key, f"idom chain from {k} did not terminate at entry; got {cur}"


def test_linear_function_one_block_dominates_self():
    """LDA #$05; STA $00; RTS — one V2Block, idom is self."""
    rom = make_lorom_bank0({
        0x8000: bytes([0xA9, 0x05, 0x85, 0x00, 0x60]),
    })
    graph = decode_function(rom, bank=0, start=0x8000, entry_m=1, entry_x=1)
    cfg = build_cfg(graph)

    assert len(cfg.blocks) == 1, f"expected 1 block, got {len(cfg.blocks)}"
    [(k, blk)] = list(cfg.blocks.items())
    assert k == graph.entry
    assert cfg.dominators[k] == k
    mnems = [di.insn.mnem for di in blk.insns]
    assert mnems == ['LDA', 'STA', 'RTS']


def test_diamond_dominance_frontier_at_join():
    """Classic diamond: cond branch into two paths converging at a join.

    $8000 BEQ $8006     ; cond branch
    $8002 LDA #$01      ; "left" arm
    $8004 BRA $8008
    $8006 LDA #$02      ; "right" arm
    $8008 RTS           ; join

    All four blocks share (m, x) = (1, 1). The join $8008 should appear
    in DF(both arms). Entry's DF is empty.
    """
    rom = make_lorom_bank0({
        0x8000: bytes([
            0xF0, 0x04,                # BEQ $8006
            0xA9, 0x01,                # LDA #$01
            0x80, 0x02,                # BRA $8008
            0xA9, 0x02,                # LDA #$02
            0x60,                      # RTS at $8008
        ]),
    })
    graph = decode_function(rom, bank=0, start=0x8000, entry_m=1, entry_x=1)
    cfg = build_cfg(graph)

    # We expect blocks at $8000, $8002, $8006, $8008.
    pcs = sorted({k.pc & 0xFFFF for k in cfg.blocks})
    assert pcs == [0x8000, 0x8002, 0x8006, 0x8008], f"got {[hex(p) for p in pcs]}"

    join_key = next(k for k in cfg.blocks if (k.pc & 0xFFFF) == 0x8008)
    left_key = next(k for k in cfg.blocks if (k.pc & 0xFFFF) == 0x8002)
    right_key = next(k for k in cfg.blocks if (k.pc & 0xFFFF) == 0x8006)
    entry_key = graph.entry

    assert join_key in cfg.dominance_frontier[left_key], "left arm DF should contain join"
    assert join_key in cfg.dominance_frontier[right_key], "right arm DF should contain join"
    assert cfg.dominance_frontier[entry_key] == frozenset()


if __name__ == '__main__':
    test_same_pc_different_mx_two_blocks()
    test_dominator_tree_handles_mode_split()
    test_linear_function_one_block_dominates_self()
    test_diamond_dominance_frontier_at_join()
    print("test_cfg_mode_split_blocks: OK")
