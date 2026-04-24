"""X-as-parameter phi-merge at forward branch targets.

When a function has `k` (or `j`) as its X parameter, and a forward
branch goes to a label that has multiple predecessors, the branch
site must pre-materialize X into a mutable local BEFORE emitting the
goto. Otherwise:

  - First-visited predecessor records X_var='k' (the param name,
    since `_materialize` returns simple values unchanged).
  - Second-visited predecessor materialized X into v<N>. Phi-merge
    then emits `k = v<N>;` on that path — but the first predecessor
    never gets a matching `v<N> = k;` sync before its goto.
  - On the first-predecessor path, v<N> is still its declaration
    default (0). X-indexed loads/stores at the merge label read
    slot 0 instead of slot k.

Canonical failure: SMW koopa-falls-through-map (2026-04-24),
HandleNormalSpriteLevelColl_019211 at ROM $019211. BEQ at $019214
skipped the fall-through's X materialize; slot-0 was read/written
at label_925b; the on-ground flag was never set at $019435; koopa
fell through the map.

Fix: at the forward-branch emitter, if self.X is still a param
name ('k'/'j'), call _ensure_mutable_x to allocate `v<N>` and emit
`v<N> = k;` BEFORE the goto. Alias-preserving (bank01.cfg's
`default_init_y = x` makes self.Y share the same expr as self.X;
_ensure_mutable_x must update Y too when aliased).
"""
import pathlib
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / 'recompiler'))

import recomp  # noqa: E402


def _is_simple_assign_to(line: str, rhs: str) -> bool:
    """Match a line of the form `<ident> = <rhs>;` (ignore indent)."""
    s = line.strip()
    if not s.endswith(f'= {rhs};'):
        return False
    lhs = s[:-len(f'= {rhs};')].strip()
    return bool(lhs) and all(c.isalnum() or c == '_' for c in lhs)


def _emit_body(rom: bytes, start: int, end: int,
               sig: str = 'void(uint8_k)') -> str:
    insns = recomp.decode_func(rom=rom, bank=0, start=start, end=end,
                               known_func_starts={start})
    lines = recomp.emit_function(
        name='test_fn', insns=insns, bank=0,
        func_names={}, func_sigs={},
        sig=sig, rom=rom, end_addr=end,
    )
    return '\n'.join(lines)


def test_beq_forward_to_x_indexed_label_premat_x_from_k():
    """BEQ skipping past an X-mutation must pre-materialize X=k.

    ROM:
        $8000: LDA $1234    A=$1234 (sets Z flag)
        $8003: BEQ $800a    forward
        $8005: LDX #$05     reassigns X
        $8007: STA $80,X    fall-through uses new X
        $8009: RTS
        $800a: STA $90,X    label: uses X (should still be k on BEQ path)
        $800c: RTS
    """
    rom = bytes([
        0xAD, 0x34, 0x12,  # $8000 LDA $1234
        0xF0, 0x05,        # $8003 BEQ $800a  (+5 from $8005)
        0xA2, 0x05,        # $8005 LDX #$05
        0x95, 0x80,        # $8007 STA $80,X
        0x60,              # $8009 RTS
        0x95, 0x90,        # $800a STA $90,X
        0x60,              # $800c RTS
    ])
    body = _emit_body(rom, 0x8000, 0x800d)
    # The fix must emit a local `v<N> = k;` BEFORE the goto so the
    # BEQ-taken path carries X=k into label_800a.
    pre_beq_lines = []
    for line in body.splitlines():
        if 'goto label_800a' in line:
            break
        pre_beq_lines.append(line)
    has_sync = any(
        _is_simple_assign_to(line, rhs='k') for line in pre_beq_lines
    )
    assert has_sync, (
        f'Expected a local-X = k; sync BEFORE the goto at BEQ $8003, '
        f'so the BEQ-taken path carries k into the merge label.\n'
        f'Generated body (pre-BEQ section):\n' + '\n'.join(pre_beq_lines) +
        f'\n\nFull body:\n{body}'
    )


def test_no_premat_when_x_already_mutable():
    """If X has already been reassigned to a local, don't re-materialize.

    Regression guard: the fix must ONLY pre-materialize when X is still
    the parameter name ('k'/'j'). If X is already in v<N>, no-op.
    """
    rom = bytes([
        0xA2, 0x03,        # $8000 LDX #$03 (X := 3, allocates v<N>)
        0xAD, 0x34, 0x12,  # $8002 LDA $1234
        0xF0, 0x02,        # $8005 BEQ $8009
        0x60,              # $8007 RTS (pad)
        0xEA,              # $8008 NOP (pad)
        0x95, 0x80,        # $8009 STA $80,X
        0x60,              # $800b RTS
    ])
    body = _emit_body(rom, 0x8000, 0x800c)
    pre_beq_lines = []
    for line in body.splitlines():
        if 'goto label_8009' in line:
            break
        pre_beq_lines.append(line)
    redundant_syncs = [
        line for line in pre_beq_lines if _is_simple_assign_to(line, rhs='k')
    ]
    assert len(redundant_syncs) == 0, (
        f'Should not emit redundant `= k;` sync when X is already mutable. '
        f'Found: {redundant_syncs}\n\nBody:\n{body}'
    )


def test_ensure_mutable_x_gives_y_separate_var_when_aliased():
    """When _ensure_mutable_x rewrites self.X from 'k' to a local, and
    self.Y was aliased to the same 'k' (e.g. via cfg default_init_y=x),
    self.Y must get its OWN separate local (not a pointer to X's new
    var).

    Rationale: if X and Y share the same var, the label-pop phi-merge
    (which emits separate X-sync then Y-sync assignments) will cross-
    contaminate — Y-sync overwrites X-sync on the shared var. That
    breaks X-indexed loads downstream (canonical failure: both-fixes
    koopa-flies-up 2026-04-24, Y-sync-overwrites-X). Giving Y its own
    var `= <param>;` keeps both tracking the original param value at
    the branch site, as the alias semantically intended, but lets
    their phi-merges at labels diverge independently.

    Also handles the k-corruption case: leaving Y as 'k' would let
    downstream phi-merges emit `k = <Y_local>;`, corrupting the
    sprite-slot param.
    """
    ctx = recomp.EmitCtx(
        bank=1, func_names={}, func_sigs={},
        init_x='k',
    )
    # Simulate default_init_y=x aliasing.
    ctx.Y = ctx.X
    assert ctx.X == 'k' and ctx.Y == 'k'
    name = ctx._ensure_mutable_x('uint8')
    assert name is not None and name != 'k'
    assert ctx.X == name, f'X should be the new var, got {ctx.X!r}'
    assert ctx.Y is not None and ctx.Y != 'k' and ctx.Y != name, (
        f'Y must get its OWN separate var (not the param, not aliased '
        f'to X). Got Y={ctx.Y!r}, X={ctx.X!r}'
    )
    # Both should have been initialized from 'k' (check the emit).
    emitted = '\n'.join(ctx.lines)
    assert f'{name} = k;' in emitted, (
        f'Expected X var init emit `{name} = k;`; got:\n{emitted}'
    )
    assert f'{ctx.Y} = k;' in emitted, (
        f'Expected separate Y var init emit `{ctx.Y} = k;`; got:\n{emitted}'
    )


def test_ensure_mutable_x_preserves_non_aliased_y():
    """Conversely: if Y is not aliased to X, _ensure_mutable_x must
    leave Y alone."""
    ctx = recomp.EmitCtx(
        bank=0, func_names={}, func_sigs={},
        init_x='k',
    )
    ctx.Y = '0x3c'  # Y was set independently (e.g. LDY #$3C)
    assert ctx.X == 'k' and ctx.Y == '0x3c'
    ctx._ensure_mutable_x('uint8')
    assert ctx.X != 'k', 'X should have been materialized'
    assert ctx.Y == '0x3c', (
        f'Y was not aliased — must not be overwritten. Got Y={ctx.Y!r}'
    )
