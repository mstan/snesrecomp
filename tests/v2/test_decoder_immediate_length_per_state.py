"""Pin v2 decoder: variable-length immediates (LDA/LDX/LDY/CMP/CPX/CPY/
ADC/SBC/AND/ORA/EOR/BIT) consume the right byte count per the entry M/X.

When the same PC is reached with different M/X, both decodings must:
  - have different `length` (2 in M=1, 3 in M=0 for M-dependent ops)
  - have different decoded `operand` (low byte vs full word)
  - emit a fall-through successor at the correct next PC

This was the v1 invariant violation: `Insn.length` was baked once with
the linear m/x at first decode, so a second reaching mode-state silently
reused the wrong length and shifted every downstream PC."""
from _helpers import make_lorom_bank0  # noqa: E402

from v2.decoder import decode_function, DecodeKey  # noqa: E402


def test_lda_imm_length_per_mode():
    """
    $8000 B0 0A    BCS $800C       ; carry-conditional → fork (m=1,x=1) -> $800C
    $8002 C2 20    REP #$20        ; clears M -> (m=0,x=1)
    $8004 80 06    BRA $800C       ; reaches $800C with (m=0,x=1)
    $800C A9 ?? ?? LDA #imm        ; decoded twice:
                                   ;   (m=1,x=1) -> length 2, operand byte
                                   ;   (m=0,x=1) -> length 3, operand word
    Then differing fall-through PCs:
    $800E (M=1) 60 RTS
    $800F (M=0) 60 RTS
    """
    blobs = {
        0x8000: bytes([
            0xB0, 0x0A,         # BCS $800C
            0xC2, 0x20,         # REP #$20
            0x80, 0x06,         # BRA $800C
        ]),
        # Pad to $800C
        0x800C: bytes([0xA9, 0x34, 0x12]),  # LDA #$1234 (or #$34 in M=1)
        0x800F: bytes([0x60]),              # RTS at the M=0 fall-through site
        0x800E: bytes([0x60]),              # RTS at the M=1 fall-through site (overlaps low byte of LDA operand — OK because decoder won't reach $800E from M=0 path)
    }
    # Resolve overlap: 0x800E overlaps with the LDA operand high byte ($12).
    # We need to ensure layout: $800C=A9, $800D=34, $800E=12, $800F=60.
    # M=1 decoder: LDA at $800C, length=2 -> next $800E. We want $800E to be a valid opcode.
    # But $800E currently = 0x12 (ORA dp_indir, 2 bytes). Fine — let's just put RTS at the M=1 path's first reachable next-pc.
    # Simplest fix: make sure $800E is a benign terminator for the M=1 path.
    # Replace the imm bytes so the layout works for both:
    #   $800C=A9, $800D=34, $800E=60 (RTS), $800F=00 (BRK -- terminates M=0 path's $800F+ chain too)
    # And for M=0, decoder at $800C reads imm=$60_34 -> length 3, next $800F. $800F=00 (BRK terminator).
    blobs = {
        0x8000: bytes([
            0xB0, 0x0A,
            0xC2, 0x20,
            0x80, 0x06,
        ]),
        0x800C: bytes([0xA9, 0x34, 0x60, 0x00]),  # LDA opcode + 2 imm bytes + a M=1 RTS at $800E + BRK at $800F
    }
    rom = make_lorom_bank0(blobs)
    graph = decode_function(rom, bank=0, start=0x8000, entry_m=1, entry_x=1)

    keys_at_lda = [k for k in graph.insns if (k.pc & 0xFFFF) == 0x800C]
    assert len(keys_at_lda) == 2, (
        f"expected $00:800C decoded twice; got {len(keys_at_lda)}"
    )

    by_mx = {(k.m, k.x): graph.insns[k] for k in keys_at_lda}
    m1 = by_mx[(1, 1)]
    m0 = by_mx[(0, 1)]

    assert m1.insn.mnem == 'LDA', f"M=1 decode: expected LDA, got {m1.insn.mnem}"
    assert m0.insn.mnem == 'LDA', f"M=0 decode: expected LDA, got {m0.insn.mnem}"

    assert m1.insn.length == 2, f"M=1 decode: expected length 2, got {m1.insn.length}"
    assert m0.insn.length == 3, f"M=0 decode: expected length 3, got {m0.insn.length}"

    # Operand: M=1 reads byte 0x34. M=0 reads word 0x6034.
    assert m1.insn.operand == 0x34, f"M=1 operand expected 0x34, got 0x{m1.insn.operand:X}"
    assert m0.insn.operand == 0x6034, f"M=0 operand expected 0x6034, got 0x{m0.insn.operand:X}"

    # Successors: M=1 falls through to $800E, M=0 falls through to $800F.
    m1_succ_pcs = {(s.pc & 0xFFFF) for s in m1.successors}
    m0_succ_pcs = {(s.pc & 0xFFFF) for s in m0.successors}
    assert 0x800E in m1_succ_pcs, f"M=1 LDA fall-through: expected $800E, got {m1_succ_pcs}"
    assert 0x800F in m0_succ_pcs, f"M=0 LDA fall-through: expected $800F, got {m0_succ_pcs}"


def test_ldx_imm_length_per_x_state():
    """LDX #imm length depends on X flag, not M flag.

    $8000 B0 0A    BCS $800C
    $8002 C2 10    REP #$10        ; clears X -> (m=1,x=0)
    $8004 80 06    BRA $800C
    $800C A2 ?? ?? LDX #imm
    """
    blobs = {
        0x8000: bytes([0xB0, 0x0A, 0xC2, 0x10, 0x80, 0x06]),
        0x800C: bytes([0xA2, 0x34, 0x60, 0x00]),
    }
    rom = make_lorom_bank0(blobs)
    graph = decode_function(rom, bank=0, start=0x8000, entry_m=1, entry_x=1)

    keys_at_ldx = [k for k in graph.insns if (k.pc & 0xFFFF) == 0x800C]
    assert len(keys_at_ldx) == 2, f"expected 2 decodes at $800C, got {len(keys_at_ldx)}"

    by_x = {k.x: graph.insns[k] for k in keys_at_ldx}
    assert by_x[1].insn.length == 2, f"X=1 LDX: expected length 2, got {by_x[1].insn.length}"
    assert by_x[0].insn.length == 3, f"X=0 LDX: expected length 3, got {by_x[0].insn.length}"
    assert by_x[1].insn.operand == 0x34
    assert by_x[0].insn.operand == 0x6034


if __name__ == '__main__':
    test_lda_imm_length_per_mode()
    test_ldx_imm_length_per_x_state()
    print("test_decoder_immediate_length_per_state: OK")
