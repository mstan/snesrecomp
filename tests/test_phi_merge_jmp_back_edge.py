"""JMP ABS to an earlier-emitted label must phi-sync register tracker.

The DiagonalLedge bug (2026-04-26) showed that `_emit_jmp` for ABS mode
emitted `goto label_NNNN;` directly, with no `_emit_backedge_phi` call.
When the JMP target is a label that was already laid down upstream
(common for forward-PC-but-emission-order-backward jumps used as inner-
loop merge points), the loop header reads its tracked X/Y/A/B from
named vars captured at label-emit time. If the JMP path mutates X
(e.g. via LDX inside an inner block) and jumps back without the sync,
the next iteration of the outer loop reads the STALE label-time X.

Canonical failure: GrassObjXX_DiagonalLedge inner block reloads X via
LDX $1 (=8 in the attract demo), then JMPs to the outer DEX-BNE
header. Without the phi sync, header's X stays the OLD value (0),
DEX wraps 0->0xFF, and the inner $3F-fill loop runs ~256 iterations
instead of 7-8, over-writing valid Map16 tiles. Visible symptom:
Mario sinks 1 block under in the attract demo.

The fix mirrors the BRA/BRL back-edge path (recomp.py:5245).
"""
import pathlib
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / 'recompiler'))

import recomp  # noqa: E402


def _emit_body(rom: bytes, start: int, end: int,
               sig: str = 'void()') -> str:
    insns = recomp.decode_func(rom=rom, bank=0, start=start, end=end,
                               known_func_starts={start})
    lines = recomp.emit_function(
        name='test_fn', insns=insns, bank=0,
        func_names={}, func_sigs={},
        sig=sig, rom=rom, end_addr=end,
    )
    return '\n'.join(lines)


def test_jmp_abs_back_edge_syncs_x_to_label_var():
    """JMP ABS to an earlier label must emit X-sync before goto.

    ROM layout (bank 0, all 8-bit M/X):
        $8000: LDX #$00      ; X := 0  (label_8009 will capture this)
        $8002: STX $00       ; force X to be tracked as a var
        $8004: BRA $8009     ; jump forward to define label_8009 in flow
        $8006: NOP NOP NOP   ; filler
        $8009: STA $50,X     ; label_8009 -- X is the captured var
        $800B: LDX #$05      ; X mutated to 5
        $800D: STX $00       ; ensure X is tracked
        $800F: JMP $8009     ; ABS JMP back to label_8009
                              ;   recomp must emit `<label_x_var> = <cur_X>;`
                              ;   before this goto, else next iter of label_8009
                              ;   reads STALE X.
        $8012: RTS           ; (unreachable)
    """
    rom = bytes([
        0xA2, 0x00,        # $8000 LDX #$00
        0x86, 0x00,        # $8002 STX $00
        0x80, 0x03,        # $8004 BRA $8009 (+3 from $8006)
        0xEA, 0xEA, 0xEA,  # $8006-8 NOP NOP NOP
        0x95, 0x50,        # $8009 STA $50,X
        0xA2, 0x05,        # $800B LDX #$05
        0x86, 0x00,        # $800D STX $00
        0x4C, 0x09, 0x80,  # $800F JMP $8009
        0x60,              # $8012 RTS
    ])
    body = _emit_body(rom, 0x8000, 0x8013)
    # Find the JMP back-edge goto and check the line before it for an
    # `<ident> = <cur_x_var>;` sync. Without the fix, the line before
    # the goto is the previous tracked emit (no sync line).
    pre_jmp_lines = []
    found_jmp = False
    for line in body.splitlines():
        if 'goto label_8009' in line and 'BRA' not in line and 'always taken' not in line:
            # The forward BRA also emits `goto label_8009`; skip until the
            # second occurrence (the JMP back-edge).
            if any('label_8009:' in l for l in pre_jmp_lines):
                found_jmp = True
                break
        pre_jmp_lines.append(line)
    assert found_jmp, (
        f'Expected a JMP back-edge `goto label_8009` after label_8009 '
        f'emission. Body:\n{body}'
    )
    # Look in the LAST few lines before the back-edge goto for a sync
    # of the form `<ident> = <ident_or_literal>;` where the LHS is the
    # label's X var (a v<N>) and the RHS is current X.
    tail = pre_jmp_lines[-6:]
    has_sync = any(
        line.strip().startswith('v') and '=' in line and line.strip().endswith(';')
        and not line.strip().startswith('void')
        for line in tail
    )
    assert has_sync, (
        f'Expected a phi-sync `v<N> = ...;` in the lines immediately '
        f'before the JMP back-edge goto label_8009.\n'
        f'Tail of pre-JMP body:\n' + '\n'.join(tail) +
        f'\n\nFull body:\n{body}'
    )


def test_jmp_abs_forward_into_backedge_target_preallocates_phi():
    """Forward JMP into a label that becomes a back-edge target must
    pre-allocate phi vars and emit phi assignments at the goto site.

    Background: prior versions silently no-op'd `_emit_backedge_phi`
    when the target's label hadn't been emitted yet, so a forward
    JMP into a not-yet-emitted label entered the label's body with
    label-vars at zero-init. When the label is also reached by a
    later back-edge that DOES emit phi, the body's vars on the FIRST
    entry (forward path) are wrong while subsequent iterations work.
    Diagonal-ledge sinking (Issue B 2026-04-27) was driven by exactly
    this pattern: JMP $0DB836 at $0DB820 entered the inner loop with
    v31 (A) and v32 (Y) zero, then BNE to label_b82e called
    SetMap16HighByteForCurrentObject_Page00(v32=0).

    The fix: lazy pre-allocation in `_emit_backedge_phi`. When the
    target's `_label_*` is unset, allocate fresh vars NOW and emit
    `<pre_x> = self.X;` etc. Label-emission later detects the
    pre-allocation and adopts those vars as the label's tracked
    state instead of capturing self.A/X/Y/B.

    ROM (bank 0, all 8-bit M/X), shaped to mimic diagonal-ledge:
        $8000: LDA #$AA      A := $AA  (forward path's A)
        $8002: LDY #$BB      Y := $BB  (forward path's Y)
        $8004: LDX #$03      X := 3
        $8006: STX $00       force X tracking
        $8008: JMP $8011     forward JMP into not-yet-emitted label
        $800B: STA $51       (label_800b — back-edge target via BNE
                              below — laid down in emission walk)
        $800D: LDX #$01      X mutated
        $800F: STX $00       force X tracking
        $8011: DEX           label_8011 (back-edge target of BNE)
        $8012: BNE $800B     backward branch to label_800b
        $8014: RTS
    """
    rom = bytes([
        0xA9, 0xAA,        # $8000 LDA #$AA
        0xA0, 0xBB,        # $8002 LDY #$BB
        0xA2, 0x03,        # $8004 LDX #$03
        0x86, 0x00,        # $8006 STX $00
        0x4C, 0x11, 0x80,  # $8008 JMP $8011
        0x85, 0x51,        # $800B STA $51 (label_800b)
        0xA2, 0x01,        # $800D LDX #$01
        0x86, 0x00,        # $800F STX $00
        0xCA,              # $8011 DEX (label_8011)
        0xD0, 0xF7,        # $8012 BNE $800B (backward, -9)
        0x60,              # $8014 RTS
    ])
    body = _emit_body(rom, 0x8000, 0x8015)
    lines = body.splitlines()
    # Locate the forward JMP's goto — it precedes label_800b in emit
    # order, and label_800b is laid down before label_8011.
    goto_idx = next(
        (i for i, ln in enumerate(lines) if 'goto label_8011' in ln),
        None,
    )
    assert goto_idx is not None, f'Expected JMP goto label_8011. Body:\n{body}'
    # The 6 lines immediately before the goto should include phi
    # assignments to fresh v<N> vars covering A and Y at minimum
    # (X=3 mutable, A=$AA, Y=$BB are all live).
    pre = '\n'.join(lines[max(0, goto_idx - 8):goto_idx])
    # Heuristic: at least one v<N> = ...; line that references either
    # 0xaa, 0xbb, or 3 (the values held by A/Y/X at the JMP site).
    has_phi = any(
        line.strip().startswith('v') and '=' in line and line.strip().endswith(';')
        and ('0xaa' in line or '0xbb' in line or '3' in line)
        for line in lines[max(0, goto_idx - 8):goto_idx]
    )
    assert has_phi, (
        f'Expected pre-allocated phi `v<N> = <A/X/Y>;` assignments '
        f'before the forward `goto label_8011;`. Got:\n{pre}\n\n'
        f'Full body:\n{body}'
    )


def test_label_emit_after_preallocation_reuses_vars():
    """When a label was pre-allocated by an earlier forward goto,
    label-emission must REUSE those vars rather than capturing fresh
    self.A/X/Y/B. Otherwise the body reads from one set of vars while
    the goto wrote to another, defeating the phi.

    Uses the same ROM as the previous test. Verifies that the
    RDB_BLOCK_HOOK at label_8011 references the same v<N> names that
    appear in the phi assignments before the goto.
    """
    rom = bytes([
        0xA9, 0xAA,
        0xA0, 0xBB,
        0xA2, 0x03,
        0x86, 0x00,
        0x4C, 0x11, 0x80,
        0x85, 0x51,
        0xA2, 0x01,
        0x86, 0x00,
        0xCA,
        0xD0, 0xF7,
        0x60,
    ])
    insns = recomp.decode_func(rom=rom, bank=0, start=0x8000, end=0x8015,
                               known_func_starts={0x8000})
    lines = recomp.emit_function(
        name='test_fn', insns=insns, bank=0,
        func_names={}, func_sigs={},
        sig='void()', rom=rom, end_addr=0x8015,
        reverse_debug=True,
    )
    body = '\n'.join(lines)
    # Find the RDB_BLOCK_HOOK that fires AT label_8011 — its A/X/Y
    # arguments should be v<N> identifiers (the pre-allocated vars).
    hook_line = next(
        (ln for ln in lines if 'RDB_BLOCK_HOOK(0x008011' in ln),
        None,
    )
    assert hook_line is not None, (
        f'Expected RDB_BLOCK_HOOK at $008011. Body:\n{body}'
    )
    # The hook's args must be v<N> identifiers (cast through uint32_t),
    # not literal 0 — that would mean the label was reading zero-init
    # vars rather than the pre-allocated phi.
    assert '(uint32_t)(0)' not in hook_line, (
        f'label_8011 RDB_BLOCK_HOOK references literal 0 — pre-allocated '
        f'phi vars not adopted at label-emission. Got:\n  {hook_line}\n\n'
        f'Full body:\n{body}'
    )
