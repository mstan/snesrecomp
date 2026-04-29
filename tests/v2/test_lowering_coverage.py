"""Pin v2 lowering: every 65816 opcode in the snes65816 opcode table has
a registered lowering rule, and every (mnem, mode) combination produces
a non-empty IR op list."""
from _helpers import make_lorom_bank0  # noqa: E402

import snes65816  # noqa: E402
from snes65816 import decode_insn  # noqa: E402
from v2 import lowering  # noqa: E402
from v2.ir import Value  # noqa: E402


def _vf():
    """Value factory closure."""
    counter = [0]
    def alloc():
        counter[0] += 1
        return Value(vid=counter[0])
    return alloc


def test_every_mnemonic_in_opcode_table_is_dispatched():
    """No mnemonic appears in snes65816's opcode table that lowering doesn't know about."""
    handled = lowering.all_known_mnemonics()
    in_table = lowering.all_opcode_mnemonics()
    missing = in_table - handled
    assert not missing, f"mnemonics in opcode table but not in lowering dispatch: {sorted(missing)}"


def test_every_opcode_lowers_to_nonempty_ir():
    """For every opcode 0x00..0xFF in the table, decode it (with reasonable
    immediate bytes) and assert lower() returns at least one IR op."""
    for opcode, (mnem, mode, _len) in snes65816._OPCODES.items():
        # Build a minimal decode-able byte sequence: opcode + up to 3 operand bytes.
        rom_bytes = bytearray(0x8000)
        rom_bytes[0] = opcode
        rom_bytes[1] = 0x10
        rom_bytes[2] = 0x20
        rom_bytes[3] = 0x30
        # Decode at $00:8000 with both M=1 and M=0 entry to exercise variable-len imm
        for m in (1, 0):
            for x in (1, 0):
                insn = decode_insn(bytes(rom_bytes), 0, 0x8000, 0, m=m, x=x)
                assert insn is not None, f"decode_insn failed for opcode 0x{opcode:02X}"
                ops = lowering.lower(insn, value_factory=_vf())
                assert ops, (
                    f"opcode 0x{opcode:02X} ({mnem} mode={mode}) at (m={m},x={x}) "
                    f"lowered to empty IR"
                )


def test_imm_lda_lowering_uses_correct_width():
    """LDA #$XX in M=1 emits an 8-bit ConstI; LDA #$XXXX in M=0 emits a 16-bit ConstI."""
    rom = bytearray(0x8000)
    rom[0:3] = bytes([0xA9, 0x34, 0x12])  # LDA #$XX (or #$1234 in M=0)
    # decode_insn doesn't stamp m_flag/x_flag — callers do (v2 decoder does).
    # Lowering reads insn.m_flag, so we set it explicitly here.
    # M=1
    insn1 = decode_insn(bytes(rom), 0, 0x8000, 0, m=1, x=1)
    insn1.m_flag, insn1.x_flag = 1, 1
    ops1 = lowering.lower(insn1, value_factory=_vf())
    from v2.ir import ConstI
    consti1 = next(o for o in ops1 if isinstance(o, ConstI))
    assert consti1.width == 1, f"M=1 LDA: expected width 1, got {consti1.width}"
    assert consti1.value == 0x34, f"M=1 LDA: expected value 0x34, got 0x{consti1.value:X}"
    # M=0
    insn0 = decode_insn(bytes(rom), 0, 0x8000, 0, m=0, x=1)
    insn0.m_flag, insn0.x_flag = 0, 1
    ops0 = lowering.lower(insn0, value_factory=_vf())
    consti0 = next(o for o in ops0 if isinstance(o, ConstI))
    assert consti0.width == 2, f"M=0 LDA: expected width 2, got {consti0.width}"
    assert consti0.value == 0x1234, f"M=0 LDA: expected value 0x1234, got 0x{consti0.value:X}"


if __name__ == '__main__':
    test_every_mnemonic_in_opcode_table_is_dispatched()
    test_every_opcode_lowers_to_nonempty_ir()
    test_imm_lda_lowering_uses_correct_width()
    print("test_lowering_coverage: OK")
