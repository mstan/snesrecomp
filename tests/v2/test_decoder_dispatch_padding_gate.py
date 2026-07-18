"""Pin v2 decoder dispatch-table validity gate.

The auto-detected dispatch-helper reader (`classify_dispatch_helper`
+ in-loop reader inside `decode_function`) can't know the true count
of entries in a PLA/PLY-indirect-JMP dispatcher's table — the count
is implicit in the asm code that loads the index. The reader walks
forward until it sees a "stop" signal (zero entry, addr below
$8000, or cross-bank for `long` tables).

But: SMW's table-overread cases land on ROM padding ($FF) or cleared
regions ($00) AFTER the true table. Those bytes pass the existing
addr-range stop conditions (e.g. $FE00 >= $8000) and become spurious
"handler" addresses. The auto-promote pass then synthesises phantom
functions at those PCs, surfaced as UNRESOLVABLE_GOTO sites in
cf_debt_report.

The validity gate `_dispatch_target_is_padding(rom, bank, pc16)`
rejects entries whose target is in 16 bytes of all-$FF or all-$00.
Encodes a real ROM fact: those bytes can't be the start of any real
handler.
"""
from _helpers import make_lorom_bank0  # noqa: E402

from types import SimpleNamespace

from v2.decoder import (  # noqa: E402
    decode_function,
    _dispatch_target_is_padding,
    _resolve_indirect_dispatch_targets,
)


def test_explicit_dispatch_target_list_preserves_null_slot():
    insn = SimpleNamespace(operand=0x8100, mnem='JSR', length=3, opcode=0xFC)
    entries = _resolve_indirect_dispatch_targets(
        b'', 0xB6, insn,
        {'count': 3, 'idx_reg': 'X', 'table_bases': (),
         'targets': (0xB69000, 0, 0xB69100)},
    )
    assert entries == [0xB69000, 0, 0xB69100]


def test_padding_gate_recognises_all_ff_target():
    rom = bytearray(0x8000)
    # Real handler at $9000.
    rom[0x9000 - 0x8000:0x9008 - 0x8000] = b'\xA9\x00\xEA\xEA\x60\xEA\xEA\xEA'
    # Padding at $FE00.
    for i in range(0xFE00, 0xFE10):
        rom[i - 0x8000] = 0xFF
    rom_bytes = bytes(rom)
    assert _dispatch_target_is_padding(rom_bytes, 0, 0xFE00) is True
    assert _dispatch_target_is_padding(rom_bytes, 0, 0x9000) is False


def test_padding_gate_recognises_all_zero_target():
    rom = bytearray(0x8000)
    # Real handler at $9000.
    rom[0x9000 - 0x8000:0x9008 - 0x8000] = b'\xA9\x00\xEA\xEA\x60\xEA\xEA\xEA'
    # Cleared region at $A000 — already zero from bytearray init.
    rom_bytes = bytes(rom)
    assert _dispatch_target_is_padding(rom_bytes, 0, 0xA000) is True
    assert _dispatch_target_is_padding(rom_bytes, 0, 0x9000) is False


def test_short_dispatch_table_stops_at_padding_entry():
    """A short dispatch table whose 3rd entry points to all-$FF padding
    must be truncated to 2 entries — the padding entry and everything
    after it is dropped.
    """
    # Layout:
    #   $8000  JSL helper                    ; bytes 22 ?? ?? ??
    #   $8004  table[0] = $9000              ; 00 90
    #   $8006  table[1] = $9100              ; 00 91
    #   $8008  table[2] = $FE00 (PADDING)    ; 00 FE  -- gate rejects
    #   $800A  table[3] = $9200 (would be valid, but truncated by gate)
    #   $9000  real handler 0
    #   $9100  real handler 1
    #   $9200  real handler 2
    #   $FE00  $FF padding
    rom = bytearray(0x8000)
    # JSL $00:E000 (we'll register $E000 as a 'short' helper)
    rom[0x8000 - 0x8000:0x8004 - 0x8000] = b'\x22\x00\xE0\x00'
    # Table after the JSL.
    rom[0x8004 - 0x8000:0x800C - 0x8000] = b'\x00\x90\x00\x91\x00\xFE\x00\x92'
    # Add an RTS so the post-table fall-through has a terminator if reached.
    rom[0x800C - 0x8000] = 0x60
    # Real handlers.
    rom[0x9000 - 0x8000:0x9001 - 0x8000] = b'\x60'   # RTS
    rom[0x9100 - 0x8000:0x9101 - 0x8000] = b'\x60'
    rom[0x9200 - 0x8000:0x9201 - 0x8000] = b'\x60'
    # Padding at $FE00.
    for i in range(0xFE00, 0xFE10):
        rom[i - 0x8000] = 0xFF
    # The dispatch helper at $E000 doesn't need real bytes — we register
    # it externally by passing dispatch_helpers={...}. decode_function
    # only reads the JSL operand to look up the helper kind; it doesn't
    # decode the helper's body.
    rom_bytes = bytes(rom)
    graph = decode_function(
        rom_bytes, bank=0, start=0x8000, entry_m=1, entry_x=1,
        dispatch_helpers={0x00E000: 'short'},
    )
    # Find the JSL insn in the graph.
    jsl_di = None
    for di in graph.insns.values():
        if di.insn.mnem == 'JSL':
            jsl_di = di
            break
    assert jsl_di is not None, "JSL not found in graph"
    entries = jsl_di.insn.dispatch_entries
    # Table truncated at the padding entry.
    assert entries == [0x9000, 0x9100], (
        f"Expected truncation at $FE00 padding entry; got {[hex(e) for e in entries]}"
    )


def test_short_dispatch_table_stops_at_zero_target():
    """An entry whose target is all-$00 also halts the table."""
    rom = bytearray(0x8000)
    # JSL helper.
    rom[0x8000 - 0x8000:0x8004 - 0x8000] = b'\x22\x00\xE0\x00'
    # table[0]=$9000, table[1]=$9100, table[2]=$A000 (cleared/zero)
    rom[0x8004 - 0x8000:0x800A - 0x8000] = b'\x00\x90\x00\x91\x00\xA0'
    rom[0x800A - 0x8000] = 0x60   # RTS after table
    rom[0x9000 - 0x8000:0x9001 - 0x8000] = b'\x60'
    rom[0x9100 - 0x8000:0x9101 - 0x8000] = b'\x60'
    # $A000+ is zero from bytearray init.
    rom_bytes = bytes(rom)
    graph = decode_function(
        rom_bytes, bank=0, start=0x8000, entry_m=1, entry_x=1,
        dispatch_helpers={0x00E000: 'short'},
    )
    jsl_di = next(di for di in graph.insns.values() if di.insn.mnem == 'JSL')
    assert jsl_di.insn.dispatch_entries == [0x9000, 0x9100]


def test_short_dispatch_table_with_all_real_targets_keeps_all():
    """Sanity: when no entry points to padding, the existing terminators
    (zero short or addr<$8000) determine the table length, NOT the
    padding gate."""
    rom = bytearray(0x8000)
    rom[0x8000 - 0x8000:0x8004 - 0x8000] = b'\x22\x00\xE0\x00'
    # 3 real entries then $0000 sentinel (skipped) and $0000 again (skipped).
    # Then $1234 (addr16 < $8000) terminates.
    rom[0x8004 - 0x8000:0x8010 - 0x8000] = (
        b'\x00\x90\x00\x91\x00\x92\x00\x00\x00\x00\x34\x12'
    )
    rom[0x8010 - 0x8000] = 0x60
    rom[0x9000 - 0x8000:0x9001 - 0x8000] = b'\x60'
    rom[0x9100 - 0x8000:0x9101 - 0x8000] = b'\x60'
    rom[0x9200 - 0x8000:0x9201 - 0x8000] = b'\x60'
    rom_bytes = bytes(rom)
    graph = decode_function(
        rom_bytes, bank=0, start=0x8000, entry_m=1, entry_x=1,
        dispatch_helpers={0x00E000: 'short'},
    )
    jsl_di = next(di for di in graph.insns.values() if di.insn.mnem == 'JSL')
    # Two zero-sentinels are kept; $1234 stops the table (addr<$8000).
    assert jsl_di.insn.dispatch_entries == [0x9000, 0x9100, 0x9200, 0, 0]
