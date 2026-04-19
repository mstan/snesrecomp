"""ROM-fall-through repair when decode order ≠ ROM order.

The decoder visits instructions in the order dictated by branch
discovery (linear runs + min(pending_fwd) after terminators), which
can differ from the instructions' ROM addresses. When `insns` is
emitted in decode order, a non-terminator insn at ROM PC X's natural
fall-through to X+length may land on DIFFERENT C code than intended
— whatever insn was decoded next, not whatever sits at X+length in
ROM.

Concrete case from HandleSPCUploads_Inner's NextByte loop:

    $8095 NextByte:  XBA
    ...
    $809f INC A                       ; fall-through to StartTransfer
    $80a0 StartTransfer: REP #$20     ; reached via BNE from $80a8 AND fall-through

The decoder emits $80a0-$80b1 BEFORE $8095-$809f because $80a0 was
first reached via BRA at $8093, and $8095 only later via the BNE
back-edge. In the C, `label_80a0:` precedes `v13++;` (the emitted INC
A at $809f), so `v13++;` falls off the function — wrong.

Fix: after each non-terminator insn, if the next decoded insn's
address doesn't match pc+length, emit `goto label_{pc+length}` (when
that target has a label).
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


def test_fallthrough_from_last_decoded_insn_into_earlier_label():
    # Synthetic reproduction of the NextByte/StartTransfer pattern.
    # Entry flag state: M=1, X=1 (default at function entry).
    #
    #   $8000 LDX #$02       A2 02       (2-byte imm under X=1)
    #   $8002 BRA $8006      80 02       (+2 from PC-after-BRA=$8004 → $8006)
    #   $8004 RTS            60          (padding, unreachable)
    #   $8005 NextByte: INX  E8          ← backward-branch target only
    #   $8006 StartTransfer: DEX  CA     ← BRA target AND fall-through from $8005
    #   $8007 BNE $8005      D0 FC       (-4 from PC-after-BNE=$8009 → $8005)
    #   $8009 RTS            60
    #
    # Decode flow:
    #   1. pc=$8000 LDX → $8002 BRA (terminator; pending_fwd={$8006})
    #      pc = min(pending_fwd) = $8006
    #   2. pc=$8006 DEX → $8007 BNE (cond; pending_fwd={$8005})
    #      → $8009 RTS (terminator; pending_fwd={$8005})
    #      pc = $8005
    #   3. pc=$8005 INX → $8006 already decoded → pending_fwd={}. Done.
    #
    # Decode order: [LDX@$8000, BRA@$8002, DEX@$8006, BNE@$8007,
    #                RTS@$8009, INX@$8005]
    #
    # INX@$8005 is the LAST insn in decode order AND non-terminator.
    # Its ROM fall-through is $8006 (DEX), which was emitted EARLIER
    # in the C (at index 2). Without the fix, the emitted INX line
    # is followed by the end of the function — falling off instead
    # of wrapping. With the fix, `goto label_8006;` follows INX.
    rom = bytes([
        0xA2, 0x02,              # $8000: LDX #$02
        0x80, 0x02,              # $8002: BRA $8006
        0x60,                    # $8004: RTS (pad; unreachable)
        0xE8,                    # $8005: INX
        0xCA,                    # $8006: DEX
        0xD0, 0xFC,              # $8007: BNE $8005
        0x60,                    # $8009: RTS
    ])
    body = _emit_body(rom, start=0x8000, end=0x800a)
    # label_8006 must exist — it's both a BRA target and the
    # physical-next target from INX.
    assert 'label_8006:' in body, (
        f'label_8006 missing from body:\n{body}'
    )
    # INX at $8005 is the LAST insn in decode order. After its emit,
    # a `goto label_8006;` must appear so ROM fall-through holds.
    lines = [l.strip() for l in body.splitlines()]
    # Find the '++;' line that corresponds to INX.
    inc_idx = None
    for i, l in enumerate(lines):
        if l.endswith('++;') and 'RecompStack' not in l:
            inc_idx = i
    assert inc_idx is not None, f'no INX-like ++; line in:\n{body}'
    remainder = '\n'.join(lines[inc_idx + 1:])
    assert 'goto label_8006' in remainder, (
        f'expected `goto label_8006;` after INX (decode-order '
        f'fall-through repair), got:\n---after INX---\n{remainder}'
        f'\n---full body---\n{body}'
    )
