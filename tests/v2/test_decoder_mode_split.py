"""Pin v2 decoder: when two predecessors with different M/X reach the
same PC, BOTH decodings are preserved (no last-writer-wins overwrite).

This is the central correctness fix vs the v1 `decode_func` which
silently drops one mode in `pending_flags`."""
from _helpers import make_lorom_bank0  # noqa: E402

# Top-level relative import OK because run_tests.py prepends tests/v2 to sys.path.

from v2.decoder import decode_function, DecodeKey  # noqa: E402


def test_branch_and_fall_through_with_different_mx_both_decoded():
    """
    $8000  C2 30        REP #$30        ; M=0, X=0
    $8002  B0 0C        BCS $8010       ; if C: branch -> $8010 with (m=0, x=0)
    $8004  E2 30        SEP #$30        ; M=1, X=1
    $8006  80 08        BRA $8010       ; -> $8010 with (m=1, x=1)
    $8010  EA           NOP             ; reached from two paths with different (m, x)
    $8011  60           RTS

    Without the fix, only one of the two decodings of $00:8010 survives.
    With the v2 worklist, both DecodeKeys live in the graph.
    """
    rom = make_lorom_bank0({
        0x8000: bytes([0xC2, 0x30, 0xB0, 0x0C, 0xE2, 0x30, 0x80, 0x08]),
        0x8010: bytes([0xEA, 0x60]),
    })
    graph = decode_function(rom, bank=0, start=0x8000, entry_m=1, entry_x=1)

    keys_at_8010 = [k for k in graph.insns if (k.pc & 0xFFFF) == 0x8010]
    assert len(keys_at_8010) == 2, (
        f"expected $00:8010 decoded twice (once per reaching mode-state); "
        f"got {len(keys_at_8010)}: {keys_at_8010}"
    )

    mx_set = {(k.m, k.x) for k in keys_at_8010}
    assert mx_set == {(0, 0), (1, 1)}, (
        f"expected mode states {{(0,0),(1,1)}} at $8010; got {mx_set}"
    )

    # And the Insn.m_flag/x_flag stamped on each is the entry mode of that key.
    for k in keys_at_8010:
        di = graph.insns[k]
        assert (di.insn.m_flag, di.insn.x_flag) == (k.m, k.x), (
            f"insn at {k} has stamped flags ({di.insn.m_flag},{di.insn.x_flag}) != entry"
        )


def test_same_pc_same_mx_only_one_record():
    """Sanity: two predecessors with the SAME (m, x) collapse to one record."""
    # $8000  EA NOP
    # $8001  EA NOP
    # $8002  80 02 BRA $8006
    # $8004  80 00 BRA $8006   (unreachable from $8000 but we use it via direct entry)
    # $8006  60 RTS
    rom = make_lorom_bank0({
        0x8000: bytes([0xEA, 0xEA, 0x80, 0x02, 0x80, 0x00, 0x60]),
    })
    graph = decode_function(rom, bank=0, start=0x8000, entry_m=1, entry_x=1)

    keys_at_8006 = [k for k in graph.insns if (k.pc & 0xFFFF) == 0x8006]
    assert len(keys_at_8006) == 1, (
        f"expected $00:8006 collapsed to single decode; got {len(keys_at_8006)}"
    )


if __name__ == '__main__':
    test_branch_and_fall_through_with_different_mx_both_decoded()
    test_same_pc_same_mx_only_one_record()
    print("test_decoder_mode_split: OK")
