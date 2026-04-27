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


def test_jmp_abs_forward_to_unemitted_label_no_phi():
    """Regression guard: JMP ABS to a forward (not-yet-emitted) label
    must not produce spurious sync. _emit_backedge_phi is a no-op when
    the target is not in self._label_x; verify no extra assignments.

    ROM:
        $8000: LDX #$03      X := 3
        $8002: STX $00       force tracking
        $8004: JMP $8008     forward JMP, target NOT yet emitted
        $8007: NOP           filler (skipped)
        $8008: STA $50,X     label_8008 (forward target)
        $800A: RTS
    """
    rom = bytes([
        0xA2, 0x03,        # $8000 LDX #$03
        0x86, 0x00,        # $8002 STX $00
        0x4C, 0x08, 0x80,  # $8004 JMP $8008
        0xEA,              # $8007 NOP
        0x95, 0x50,        # $8008 STA $50,X
        0x60,              # $800A RTS
    ])
    body = _emit_body(rom, 0x8000, 0x800b)
    # Find the goto and inspect the immediately preceding line.
    lines = body.splitlines()
    goto_idx = next(
        (i for i, ln in enumerate(lines) if 'goto label_8008' in ln),
        None,
    )
    assert goto_idx is not None, f'Expected a JMP goto label_8008. Body:\n{body}'
    prev = lines[goto_idx - 1].strip() if goto_idx > 0 else ''
    # Forward JMP (target unknown) — no `_label_x` entry yet, so no sync.
    # Allow comments / hooks / register tracking, but not a v<N> = ... sync
    # of the form expected by the back-edge test.
    assert not (prev.startswith('v') and prev.endswith(';') and '=' in prev
                and 'tmp' not in prev and 'RDB_' not in prev), (
        f'Forward JMP should not emit a phi sync; got:\n  {prev}\n\n'
        f'Body:\n{body}'
    )
