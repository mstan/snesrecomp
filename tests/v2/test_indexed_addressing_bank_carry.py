"""Pin the bank-carry semantics of ABS_X / ABS_Y / LONG_X / LONG_Y
indexed addressing modes (2026-05-17 class fix).

When `STA $7E2000,X` runs with X = 0xFF10, hardware computes a 24-bit
effective address `$7E:2000 + 0xFF10` with carry propagation across
the bank boundary — the store lands at `$7F:1F10`, NOT `$7E:1F10`.

The pre-fix `_segref_addr_expr` emitted
    cpu_write16(cpu, 0x7e, (uint16)(0x2000 + cpu->X), v);
which truncated the indexed address to 16 bits and left the bank
hard-coded to `0x7e`, silently dropping the carry. That clobbered
$7E:1F11 (submodule_index) during the Zelda intro's
Intro_Clear1kbBlocksOfWRAM loop — root cause of the Nintendo-jingle
endless loop.

These tests assert the emit ALWAYS routes indexed long / ABS_X / ABS_Y
writes through a 24-bit effective with `(uint8)(eff >> 16)` for the
bank arg and `(uint16)(eff)` for the address arg. Non-indexed bases
keep the cheap hard-coded bank form.
"""
from _helpers import make_lorom_bank0  # noqa: E402

from v2.emit_function import emit_function  # noqa: E402


def test_long_x_write_emits_24bit_effective_with_bank_carry():
    """`STA $7E2000,X` should produce a 24-bit effective address that
    carries the bank through `(uint8)(eff >> 16)`."""
    # $8000: REP #$30        ; m=0 x=0 (16-bit A, X)
    # $8002: A2 10 FF        ; LDX #$FF10
    # $8005: A9 00 00        ; LDA #$0000
    # $8008: 9F 00 20 7E     ; STA $7E2000,X
    # $800C: 60              ; RTS
    rom = make_lorom_bank0({
        0x8000: bytes([0xC2, 0x30,
                       0xA2, 0x10, 0xFF,
                       0xA9, 0x00, 0x00,
                       0x9F, 0x00, 0x20, 0x7E,
                       0x60]),
    })
    src = emit_function(rom, bank=0, start=0x8000, entry_m=1, entry_x=1)

    # Must NOT emit the broken hardcoded-bank form.
    assert 'cpu_write16(cpu, 0x7e, (uint16)(0x2000 + cpu->X)' not in src, \
        f'pre-fix bank-loss form re-introduced:\n{src}'
    # Must emit a 24-bit effective with the bank carry.
    assert '(uint8)(' in src and ' >> 16' in src, src
    assert '0x7e2000' in src or '0x7E2000' in src, src
    # The cpu_write16 call should reference the carried bank, not 0x7e
    # directly.
    assert 'cpu_write16(cpu, (uint8)' in src, src


def test_abs_x_write_emits_24bit_effective_with_bank_carry():
    """`STA $C800,X` (ABS_X) should also carry the bank from `DB + carry`."""
    # $8000: REP #$30
    # $8002: A2 00 80        ; LDX #$8000
    # $8005: A9 00 00        ; LDA #$0000
    # $8008: 9D 00 C8        ; STA $C800,X  (ABS_X, opcode 9D)
    # $800B: 60              ; RTS
    rom = make_lorom_bank0({
        0x8000: bytes([0xC2, 0x30,
                       0xA2, 0x00, 0x80,
                       0xA9, 0x00, 0x00,
                       0x9D, 0x00, 0xC8,
                       0x60]),
    })
    src = emit_function(rom, bank=0, start=0x8000, entry_m=1, entry_x=1)

    # The broken DB-truncate form must be gone.
    assert 'cpu_write16(cpu, cpu->DB, (uint16)(0xc800 + cpu->X)' not in src, src
    # 24-bit effective with carry-bearing bank.
    assert 'cpu->DB' in src and '<< 16' in src, src
    assert 'cpu_write16(cpu, (uint8)' in src, src


def test_non_indexed_abs_keeps_cheap_form():
    """`STA $C800` (no index) needs no bank carry — keep the cheap hardcoded
    `cpu->DB` form so we don't regress codegen for the common case."""
    # $8000: A9 42            ; LDA #$42 (m=1)
    # $8002: 8D 00 C8         ; STA $C800
    # $8005: 60               ; RTS
    rom = make_lorom_bank0({
        0x8000: bytes([0xA9, 0x42,
                       0x8D, 0x00, 0xC8,
                       0x60]),
    })
    src = emit_function(rom, bank=0, start=0x8000, entry_m=1, entry_x=1)

    # Non-indexed ABS uses the simple form.
    assert 'cpu_write8(cpu, cpu->DB, (uint16)(0xc800)' in src, src
    # Should NOT carry a 24-bit shift here (no index = no carry possible).
    assert 'cpu->DB << 16' not in src, src


def test_long_x_read_emits_24bit_effective_with_bank_carry():
    """LDA-side should mirror STA: indexed long reads also need the
    bank carry. Otherwise an off-bank read returns wrong-bank data."""
    # $8000: C2 30
    # $8002: A2 10 FF        ; LDX #$FF10
    # $8005: BF 00 20 7E     ; LDA $7E2000,X  (LONG_X read, opcode BF)
    # $8009: 60              ; RTS
    rom = make_lorom_bank0({
        0x8000: bytes([0xC2, 0x30,
                       0xA2, 0x10, 0xFF,
                       0xBF, 0x00, 0x20, 0x7E,
                       0x60]),
    })
    src = emit_function(rom, bank=0, start=0x8000, entry_m=1, entry_x=1)

    assert 'cpu_read16(cpu, 0x7e, (uint16)(0x2000 + cpu->X)' not in src, src
    assert 'cpu_read16(cpu, (uint8)' in src, src
