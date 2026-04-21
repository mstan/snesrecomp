"""Pin: _insns_read_reg_post_jsr must follow the function CFG, not just
fall-through. The canonical real-world failure is StdObj05_Coins
($0DA8C3): a do-while loop body that ends in `JSR HHSCCO ; DEC _2 ; LDA
_2 ; BPL loop_start`. Each loop iteration's JSR consumes the Y register
left by the previous iteration's JSR — but a fall-through-only walk hits
the BPL back-edge and stops without seeing any Y reader, so HHSCCO
never gets promoted to RetAY. With the sig left as `uint8(...)`, the
caller's symbolic Y tracker never updates, and every loop iteration's
JSR lands at the SAME WRAM address. The visible symptom in SMW is
column-fill objects (ground tiles) writing only 1 of 16 rows per column.

Two regression cases below: one for the SAME-target loop-back (the SMW
case), and one for "JSR ; BNE skip ; ... TYA ; STA" (a basic
fall-through positive case that must still work).
"""
import pathlib
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / 'recompiler'))

import recomp  # noqa: E402
from snes65816 import Insn, IMP, DP  # noqa: E402


def _ins(addr, mnem, mode=0, operand=0, length=1):
    """Build a minimal Insn for the analyzer's purposes."""
    i = Insn(addr=addr, opcode=0, mnem=mnem, mode=mode, operand=operand, length=length)
    return i


def test_loop_back_to_same_jsr_target_counts_as_y_consumer():
    """Caller layout (the StdObj05_Coins shape):

        $A8D8: NOP             ; loop entry, no Y read here
        $A92E: NOP             ; (some setup, would be LDA in real code)
        $A93E: JSR HHSCCO ($A95B)
        $A943: DEC _2          ; doesn't touch Y
        $A944: LDA _2          ; doesn't touch Y
        $A946: BPL $A8D8       ; loop back

    HHSCCO ($A95B) reads Y as live-in (its first insn is STA [Map16],Y).
    The walker, starting after the JSR at $A943, must follow the BPL
    back-edge to $A8D8, walk forward again, and detect the JSR-to-HHSCCO
    at $A93E as a Y consumer (because HHSCCO reads Y as live-in).
    """
    insns = [
        _ins(0x0DA8D8, 'NOP'),
        _ins(0x0DA92E, 'NOP'),
        _ins(0x0DA93E, 'JSR', operand=0xA95B, length=3),
        _ins(0x0DA943, 'DEC', mode=DP, operand=0x02, length=2),  # writes nothing in {A,X,Y}
        _ins(0x0DA944, 'LDA', mode=DP, operand=0x02, length=2),  # writes A
        _ins(0x0DA946, 'BPL', operand=0x0DA8D8, length=2),
    ]
    livein = {0xA95B: {'Y'}}
    assert recomp._insns_read_reg_post_jsr(
        insns, jsr_pc=0x0DA93E, target=0xA95B, reg='Y',
        reg_livein_by_addr=livein) is True


def test_y_write_between_jsr_and_loop_back_kills_consumer():
    """Same shape, but a TAY between the JSR and the back-edge defines Y
    locally. The walker must NOT count the next-iteration's JSR as a
    consumer because the local TAY supplied a fresh Y."""
    insns = [
        _ins(0x0DA8D8, 'NOP'),
        _ins(0x0DA93E, 'JSR', operand=0xA95B, length=3),
        _ins(0x0DA941, 'TAY'),  # writes Y
        _ins(0x0DA942, 'BRA', operand=0x0DA8D8, length=2),
    ]
    livein = {0xA95B: {'Y'}}
    assert recomp._insns_read_reg_post_jsr(
        insns, jsr_pc=0x0DA93E, target=0xA95B, reg='Y',
        reg_livein_by_addr=livein) is False


def test_simple_fall_through_y_read_still_detected():
    """Original positive case: JSR ; TYA ; STA. No CFG-walk needed,
    must still report consumer."""
    insns = [
        _ins(0x008000, 'JSR', operand=0x9000, length=3),
        _ins(0x008003, 'TYA'),                     # reads Y
        _ins(0x008004, 'STA', operand=0x10, length=2),
        _ins(0x008006, 'RTS'),
    ]
    assert recomp._insns_read_reg_post_jsr(
        insns, jsr_pc=0x008000, target=0x9000, reg='Y') is True


def test_no_y_consumer_returns_false():
    """JSR ; LDA ; STA ; RTS — Y never read. Must return False."""
    insns = [
        _ins(0x008000, 'JSR', operand=0x9000, length=3),
        _ins(0x008003, 'LDA', operand=0x10, length=2),
        _ins(0x008005, 'STA', operand=0x12, length=2),
        _ins(0x008007, 'RTS'),
    ]
    assert recomp._insns_read_reg_post_jsr(
        insns, jsr_pc=0x008000, target=0x9000, reg='Y') is False


def test_jmp_tail_call_wrapper_inherits_y_clobber():
    """`ExtObjXX_LargeBush_HandleOverlappingBigBushTiles` shape: a small
    wrapper that ends in `JMP CODE_0DA95B` (tail call). The wrapper's
    body has no JSR — only branches and a final JMP. From the wrapper's
    caller's perspective, the wrapper is a Y-clobber pass-through (the
    callee's RTS returns directly to the caller, with the callee's Y).

    The promoter's `_y_effective_clobber` must recognize JMP-to-known-func
    as a tail call edge and inherit the tail-callee's Y-clobber. Without
    this, the wrapper stays uint8(...) and bush placement loops fail
    the same way the HHSCCO loops did before the loop-Y fix."""
    import importlib
    # Build a synthetic bank cfg + decode pipeline. Easier: use the real
    # bank0d.cfg + ROM, then assert the wrapper got promoted to RetAY.
    bank0d_cfg = REPO.parent / 'recomp' / 'bank0d.cfg'
    rom_path = REPO.parent / 'smw.sfc'
    if not bank0d_cfg.exists() or not rom_path.exists():
        # In CI / fresh checkout without the ROM, skip silently — the
        # smaller synthetic cases above still pin the analyzer.
        return
    rom = open(rom_path, 'rb').read()
    if len(rom) % 1024 == 512:
        rom = rom[512:]
    cfg = recomp.parse_config(str(bank0d_cfg))
    recomp.augment_cfg_sigs_from_livein(rom, cfg)
    # ExtObjXX_LargeBush_HandleOverlappingBigBushTiles @ $0D:$A78D.
    addr = (0x0D << 16) | 0xA78D
    sig = cfg.sigs.get(addr)
    assert sig is not None, 'wrapper sig missing'
    ret, _params = recomp.parse_sig(sig)
    assert ret == 'RetAY', (
        f'wrapper sig should auto-promote to RetAY (it tail-calls HHSCCO '
        f'which is RetAY); got {sig}'
    )


def test_branch_target_y_read_detected():
    """JSR ; BEQ skip ; (Y untouched) ; skip: TYA. The Y read on the
    branch-target side must be detected even though the fall-through
    side has no Y read."""
    insns = [
        _ins(0x008000, 'JSR', operand=0x9000, length=3),
        _ins(0x008003, 'BEQ', operand=0x008010, length=2),
        _ins(0x008005, 'NOP'),
        _ins(0x008006, 'RTS'),
        _ins(0x008010, 'TYA'),  # reads Y on branch-taken path
        _ins(0x008011, 'RTS'),
    ]
    assert recomp._insns_read_reg_post_jsr(
        insns, jsr_pc=0x008000, target=0x9000, reg='Y') is True
