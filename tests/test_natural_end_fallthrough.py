"""Fall-through emit must require a TRUE fall-through landing at end_addr.

Pins the predicate that closes Issue C (Yoshi-floats-up). Phantom auto-
promotion at $01:ECEC capped Spr035_Yoshi's end_addr mid-instruction
(inside a JSR's operand bytes). The body decoder included the JSR fully,
its physical fall-through landed at $end_addr+1, but the existing
emit_function logic checked only "is the highest-PC insn terminal?" — saw
the JSR was non-terminal, and unconditionally emitted a fall-through
call to auto_01_ECEC. auto_01_ECEC ran an on-ground Y-velocity reset
every frame, defeating gravity.

The predicate that fixes this:

    Fall-through emit fires ONLY IF some non-terminal instruction `i` in
    the body satisfies `(i.addr + i.length) & 0xFFFF == end_addr`.

i.e. some instruction's natural ROM fall-through lands EXACTLY on the
next function's entry. If `end_addr` is mid-instruction (Yoshi case) or
no body instruction reaches it via fall-through (RTS-stub shape), no
fall-through is emitted.

Three failed framework attempts on 2026-04-26 are documented in
ISSUES.md; this predicate intentionally distinguishes the cases that
broke each attempt:

  - SprStatus06 RTS-stub: tiny body that's terminal at end_addr-1.
    First attempt rejected the auto_promote → broke shell-pop. This
    predicate keeps the suppression at the emit layer instead.
  - BigBoo / Yoshi-egg dispatch handlers: real fall-through into a
    dispatch_only next_func. Second + third attempts broke these.
    This predicate keeps the emit because (addr+length)==end_addr.
  - Spr035_Yoshi: phantom end_addr mid-instruction. This predicate
    suppresses because the JSR's fall-through lands past end_addr.
"""
import pathlib
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / 'recompiler'))

import recomp  # noqa: E402


def _emit(rom: bytes, start: int, end: int, *, sig='void()',
          next_func=None) -> str:
    insns = recomp.decode_func(rom=rom, bank=0, start=start, end=end,
                                known_func_starts={start})
    func_names = {}
    func_sigs = {}
    if next_func:
        nf_addr, nf_name, nf_sig = next_func
        func_names[nf_addr] = nf_name
        func_sigs[nf_addr] = nf_sig
        nf_pair = (nf_name, nf_sig)
    else:
        nf_pair = None
    lines = recomp.emit_function(
        name='test_fn', insns=insns, bank=0,
        func_names=func_names, func_sigs=func_sigs,
        sig=sig, rom=rom, end_addr=end, next_func=nf_pair,
    )
    return '\n'.join(lines)


# ---------------------------------------------------------------------------
# Case 1: Yoshi-shape. end_addr lands MID-INSTRUCTION (inside a JSR's
# operand bytes). Fall-through must be suppressed.
# ---------------------------------------------------------------------------
def test_yoshi_shape_mid_instruction_end_addr_suppresses_fallthrough():
    # Layout (entry M=1, X=1) — linear walk straddling end_addr:
    #   $8000 LDA #$01      A9 01     (2 bytes, non-terminal)
    #   $8002 STA $10       85 10     (2 bytes, non-terminal)
    #   $8004 JSR $9000     20 00 90  (3 bytes, non-terminal)
    #                                  fall-through to $8007
    #
    # end_addr = $8006 (mid-JSR-operand: phantom promotion artifact —
    # the auto-promoted phantom function landed inside JSR's bytes).
    # The JSR at $8004 has length 3 → fall-through lands at $8007, NOT
    # $8006. So no body insn falls through exactly to end_addr.
    # A fall-through emit here would call into the phantom-promoted body.
    rom = bytearray(0x10000)
    rom[0x0000] = 0xA9; rom[0x0001] = 0x01      # LDA #$01
    rom[0x0002] = 0x85; rom[0x0003] = 0x10      # STA $10
    rom[0x0004] = 0x20; rom[0x0005] = 0x00; rom[0x0006] = 0x90  # JSR $9000
    # Phantom callee at end_addr=$8006: the byte 0x90 is also operand-hi
    # of the JSR, but auto-promote saw it as code. Provide a benign body.
    rom[0x0007] = 0x60                          # RTS

    body = _emit(bytes(rom), start=0x8000, end=0x8006,
                 next_func=(0x8006, 'phantom_callee', 'void()'))

    # The bug: a `phantom_callee(...)` call appears with `/* fall-through */`
    # comment. The fix: no such call in the body.
    assert 'phantom_callee' not in body, (
        f'Yoshi-shape: phantom_callee fall-through call must NOT be emitted '
        f'when end_addr lands mid-instruction. Got body:\n{body}'
    )


# ---------------------------------------------------------------------------
# Case 2: Legit fall-through. A non-terminal insn's natural ROM fall-
# through lands EXACTLY at end_addr. Emit must fire (preserves
# Yoshi-egg-spawn / BigBoo dispatch / similar real handlers).
# ---------------------------------------------------------------------------
def test_legit_fallthrough_at_end_addr_still_emits():
    # Layout:
    #   $8000 LDA #$01      A9 01    (2 bytes)
    #   $8002 STA $10       85 10    (2 bytes, non-terminal)
    #   $8004 <next func>            (end_addr, contiguous)
    #
    # STA at $8002 has length 2 → addr+length = $8004 = end_addr exactly.
    # Real fall-through into next func.
    rom = bytearray(0x10000)
    rom[0x0000] = 0xA9; rom[0x0001] = 0x01
    rom[0x0002] = 0x85; rom[0x0003] = 0x10
    rom[0x0004] = 0x60                          # RTS at $8004 (next func body)

    body = _emit(bytes(rom), start=0x8000, end=0x8004,
                 next_func=(0x8004, 'real_next', 'void()'))

    assert 'real_next(' in body, (
        f'Legit shape: when a non-terminal insn falls through exactly at '
        f'end_addr, the fall-through call MUST be emitted. Got body:\n{body}'
    )


# ---------------------------------------------------------------------------
# Case 3: RTS-stub at end_addr-1. Body's only instruction terminates at
# end_addr boundary. No fall-through.
# ---------------------------------------------------------------------------
def test_rts_stub_at_end_boundary_suppresses_fallthrough():
    # Layout:
    #   $8000 RTS           60       (1 byte, terminal; addr+length = $8001)
    #   end_addr = $8001
    #   $8001 <next func>
    #
    # RTS is terminal, so even though addr+length = end_addr, the
    # predicate's "non-terminal" requirement excludes it. Suppress.
    rom = bytearray(0x10000)
    rom[0x0000] = 0x60                          # RTS
    rom[0x0001] = 0x60                          # next func body (RTS)

    body = _emit(bytes(rom), start=0x8000, end=0x8001,
                 next_func=(0x8001, 'next_after_rts', 'void()'))

    assert 'next_after_rts' not in body, (
        f'RTS-stub shape: terminal at end_addr-1 must NOT emit fall-through. '
        f'Got body:\n{body}'
    )


# ---------------------------------------------------------------------------
# Case 4: Mixed body — has internal RTS but ALSO a real fall-through path
# reaching end_addr. The fall-through emit MUST still fire (the RTS is
# just a sub-handler exit, not the function's terminator).
# ---------------------------------------------------------------------------
def test_internal_rts_plus_real_fallthrough_emits():
    # Layout:
    #   $8000 LDA #$01      A9 01    (2 bytes)
    #   $8002 BEQ $8007     F0 03    (cond, target $8007)
    #   $8004 RTS           60       (early exit on !zero)
    #   $8005 LDA #$02      A9 02    (2 bytes; reached via BEQ from $8002)
    #   $8007 STA $10       85 10    (2 bytes; addr+length = $8009 = end_addr)
    #   end_addr = $8009
    rom = bytearray(0x10000)
    rom[0x0000] = 0xA9; rom[0x0001] = 0x01
    rom[0x0002] = 0xF0; rom[0x0003] = 0x03
    rom[0x0004] = 0x60
    rom[0x0005] = 0xA9; rom[0x0006] = 0x02
    rom[0x0007] = 0x85; rom[0x0008] = 0x10
    rom[0x0009] = 0x60                          # next func body

    body = _emit(bytes(rom), start=0x8000, end=0x8009,
                 next_func=(0x8009, 'tail_callee', 'void()'))

    assert 'tail_callee(' in body, (
        f'Mixed shape: internal RTS does not suppress the real fall-through '
        f'at end_addr. Got body:\n{body}'
    )


if __name__ == '__main__':
    test_yoshi_shape_mid_instruction_end_addr_suppresses_fallthrough()
    test_legit_fallthrough_at_end_addr_still_emits()
    test_rts_stub_at_end_boundary_suppresses_fallthrough()
    test_internal_rts_plus_real_fallthrough_emits()
    print('OK')
