"""Pin v2 decoder: REP/SEP bitmasks update M and X independently.

`REP #$20` clears M only (X unchanged). `REP #$10` clears X only.
`REP #$30` clears both. SEP is symmetric. Successor (m, x) reflects
this per-bit semantics."""
from _helpers import make_lorom_bank0  # noqa: E402

from v2.decoder import decode_function, post_mx  # noqa: E402
from snes65816 import decode_insn  # noqa: E402


def _decode_one(opcode_bytes: bytes, m: int, x: int):
    """Decode the single instruction in `opcode_bytes` at $00:8000 with entry (m, x)."""
    rom = bytearray(0x8000)
    rom[0:len(opcode_bytes)] = opcode_bytes
    insn = decode_insn(bytes(rom), 0, 0x8000, 0, m=m, x=x)
    assert insn is not None
    return insn


def test_rep_20_clears_m_only():
    insn = _decode_one(bytes([0xC2, 0x20]), m=1, x=1)
    assert post_mx(insn, 1, 1) == (0, 1)


def test_rep_10_clears_x_only():
    insn = _decode_one(bytes([0xC2, 0x10]), m=1, x=1)
    assert post_mx(insn, 1, 1) == (1, 0)


def test_rep_30_clears_both():
    insn = _decode_one(bytes([0xC2, 0x30]), m=1, x=1)
    assert post_mx(insn, 1, 1) == (0, 0)


def test_sep_20_sets_m_only():
    insn = _decode_one(bytes([0xE2, 0x20]), m=0, x=0)
    assert post_mx(insn, 0, 0) == (1, 0)


def test_sep_10_sets_x_only():
    insn = _decode_one(bytes([0xE2, 0x10]), m=0, x=0)
    assert post_mx(insn, 0, 0) == (0, 1)


def test_sep_30_sets_both():
    insn = _decode_one(bytes([0xE2, 0x30]), m=0, x=0)
    assert post_mx(insn, 0, 0) == (1, 1)


def test_unrelated_bits_dont_touch_m_or_x():
    # bit 0x04 is the I (interrupt-disable) flag — must NOT touch M or X.
    insn = _decode_one(bytes([0xC2, 0x04]), m=1, x=1)
    assert post_mx(insn, 1, 1) == (1, 1)
    insn = _decode_one(bytes([0xE2, 0x04]), m=0, x=0)
    assert post_mx(insn, 0, 0) == (0, 0)


def test_successor_state_propagates_through_rep_sep_chain():
    """REP/SEP cascade: post-state of one feeds entry-state of next.

    $8000 C2 30   REP #$30    ; M=1,X=1 -> M=0,X=0
    $8002 E2 10   SEP #$10    ; M=0,X=0 -> M=0,X=1
    $8004 C2 20   REP #$20    ; M=0,X=1 -> M=0,X=1   (no-op, M already 0)
    $8006 E2 20   SEP #$20    ; M=0,X=1 -> M=1,X=1
    $8008 60      RTS
    """
    rom = make_lorom_bank0({
        0x8000: bytes([0xC2, 0x30, 0xE2, 0x10, 0xC2, 0x20, 0xE2, 0x20, 0x60]),
    })
    graph = decode_function(rom, bank=0, start=0x8000, entry_m=1, entry_x=1)

    def entry_at(pc):
        keys = [k for k in graph.insns if (k.pc & 0xFFFF) == pc]
        assert len(keys) == 1, f"expected single decode at ${pc:04X}, got {keys}"
        k = keys[0]
        return (k.m, k.x)

    assert entry_at(0x8000) == (1, 1)
    assert entry_at(0x8002) == (0, 0)
    assert entry_at(0x8004) == (0, 1)
    assert entry_at(0x8006) == (0, 1)
    assert entry_at(0x8008) == (1, 1)


if __name__ == '__main__':
    test_rep_20_clears_m_only()
    test_rep_10_clears_x_only()
    test_rep_30_clears_both()
    test_sep_20_sets_m_only()
    test_sep_10_sets_x_only()
    test_sep_30_sets_both()
    test_unrelated_bits_dont_touch_m_or_x()
    test_successor_state_propagates_through_rep_sep_chain()
    print("test_decoder_repsep_independent_bits: OK")
