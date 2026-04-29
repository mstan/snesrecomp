"""Pin ssa_placement.py: per-register def classification + Cytron
worklist phi placement.
"""
import pathlib
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / 'recompiler'))

import recomp  # noqa: E402
from cfg import build_cfg  # noqa: E402
from ssa_placement import (
    defines_register,
    compute_register_defs,
    compute_phi_placements,
)


def _build(rom: bytes, start: int, end: int):
    insns = recomp.decode_func(rom=rom, bank=0, start=start, end=end,
                                known_func_starts={start})
    decoded_pcs = {(i.addr & 0xFFFF) for i in insns}
    BRANCH_MNEMS = ('BPL', 'BMI', 'BEQ', 'BNE', 'BCC', 'BCS', 'BVS',
                    'BVC', 'BRA', 'BRL', 'JMP')
    valid_branch_targets = {ins.operand for ins in insns
                             if ins.mnem in BRANCH_MNEMS
                             and ins.operand in decoded_pcs}
    cfg = build_cfg(insns, valid_branch_targets, bank=0, func_start=start)
    return cfg, insns


def test_lda_defines_a_not_x():
    """LDA defines A but not X."""
    rom = bytes([0xA9, 0x05, 0x60])
    cfg, insns = _build(rom, 0x8000, 0x8003)
    lda = insns[0]
    assert defines_register(lda, 'A') is True
    assert defines_register(lda, 'X') is False
    assert defines_register(lda, 'Y') is False
    assert defines_register(lda, 'B') is True   # 16-bit-A high byte (conservative)


def test_inx_dex_define_x():
    rom = bytes([0xE8, 0xCA, 0x60])  # INX; DEX; RTS
    cfg, insns = _build(rom, 0x8000, 0x8003)
    assert defines_register(insns[0], 'X') is True   # INX
    assert defines_register(insns[1], 'X') is True   # DEX
    assert defines_register(insns[0], 'Y') is False


def test_inc_acc_defines_a_inc_dp_does_not():
    """INC A (mode == ACC) defines A; INC $00 (DP) does NOT."""
    rom = bytes([
        0x1A,        # INC A  (acc mode)
        0xE6, 0x10,  # INC $10 (DP)
        0x60,
    ])
    cfg, insns = _build(rom, 0x8000, 0x8004)
    inc_a, inc_dp = insns[0], insns[1]
    assert defines_register(inc_a, 'A') is True
    assert defines_register(inc_dp, 'A') is False


def test_diamond_phi_for_a_at_merge():
    """Both arms LDA → A def in both → phi for A at merge."""
    rom = bytes([
        0xA5, 0x00,        # $8000 LDA $00
        0xF0, 0x04,        # $8002 BEQ $8008
        0xA9, 0xAA,        # $8004 LDA #$AA
        0x80, 0x02,        # $8006 BRA $800A
        0xA9, 0xBB,        # $8008 LDA #$BB
        0x85, 0x50,        # $800A STA $50
        0x60,
    ])
    cfg, _ = _build(rom, 0x8000, 0x800D)
    defs = compute_register_defs(cfg)
    placements = compute_phi_placements(cfg, defs)
    assert 0x800A in placements['A']


def test_loop_phi_for_x_at_body():
    """LDX/DEX defines X in head and body; body has 2 preds; phi for X."""
    rom = bytes([
        0xA2, 0x05,        # $8000 LDX #$05
        0xCA,              # $8002 DEX
        0xD0, 0xFD,        # $8003 BNE $8002
        0x60,
    ])
    cfg, _ = _build(rom, 0x8000, 0x8006)
    defs = compute_register_defs(cfg)
    placements = compute_phi_placements(cfg, defs)
    assert 0x8002 in placements['X']


def test_no_phi_for_linear_function():
    rom = bytes([0xA9, 0x05, 0x85, 0x00, 0x60])
    cfg, _ = _build(rom, 0x8000, 0x8005)
    defs = compute_register_defs(cfg)
    placements = compute_phi_placements(cfg, defs)
    assert placements['A'] == frozenset()
    assert placements['X'] == frozenset()


def test_x_restores_callee_creates_x_def_at_jsr_block():
    """REGRESSION (koopa-shell): function calls a JSR whose callee has
    x_restores. SSA placement must count the JSR's containing block
    as an X def so phi gets placed at downstream merges. Without this,
    the goto-arrived path's X drifts to whatever the linear-walk
    fall-through value was (the JSR's x_restore expression).

    Repro: BEQ early-exits to merge directly (X = k); fall-through
    arm calls JSR with x_restore (X redefined), then BRAs to merge.
    Two reaching values for X at merge → phi needed.
    """
    rom = bytes([
        0xA5, 0x00,        # $8000 LDA $00
        0xF0, 0x05,        # $8002 BEQ $8009  (BEQ-arrived: X = k)
        0x20, 0x20, 0x80,  # $8004 JSR $8020  (x_restoring callee)
        0x80, 0x00,        # $8007 BRA $8009  (post-JSR: X = x_restore)
        0x95, 0x50,        # $8009 STA $50,X  merge (X-indexed)
        0x60,              # $800B RTS
    ])
    cfg, _ = _build(rom, 0x8000, 0x800C)
    # Callee at $0:8020 has x_restores
    defs = compute_register_defs(
        cfg,
        x_restores_callees={0x008020},
        bank=0,
    )
    placements = compute_phi_placements(cfg, defs)
    assert 0x8009 in placements['X'], (
        f'expected X phi at merge $8009 because JSR-block is an X def '
        f'(x_restores callee). Got X placements: {sorted(placements["X"])}'
    )


def test_x_restores_only_used_when_callee_in_set():
    """JSR to a callee NOT in x_restores_callees set is NOT an X def."""
    rom = bytes([
        0xA5, 0x00,        # $8000
        0xF0, 0x05,        # $8002 BEQ $8009
        0x20, 0x20, 0x80,  # $8004 JSR $8020
        0x80, 0x00,        # $8007 BRA $8009
        0x95, 0x50,        # $8009
        0x60,
    ])
    cfg, _ = _build(rom, 0x8000, 0x800C)
    # Empty x_restores_callees set — no synthetic X def
    defs = compute_register_defs(cfg, x_restores_callees=set(), bank=0)
    placements = compute_phi_placements(cfg, defs)
    assert 0x8009 not in placements['X']


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
