"""snesrecomp.recompiler.v2.pha_rts_autoroute

Auto-detect the canonical PHA-RTS dispatch idiom and synthesise the
matching `indirect_dispatch` directive (so emit_function emits a
bounded switch instead of leaving the PHA as a literal stack write
that the next RTS in the caller chain pops as a bogus return address).

Pattern (8 bytes, instruction-aligned inside a decoded function body):

    B9 LL HH    LDA $HHLL, Y      ; load 16-bit function pointer from table
    3A          DEC A             ; pre-decrement (RTS will +1 on pop)
    48          PHA               ; push as fake return address
    E2 30       SEP #$30          ; A,X,Y -> 8
    60          RTS               ; pops + 1 -> dispatches to function

Y holds (logical_index * 2) at this point — the caller has done a
DEC / ASL / TAY sequence (or similar) to convert a 1-based type code
into a word offset. The dispatch table at $HHLL is a parallel array of
16-bit function pointers in the same bank.

The recompiler can't infer this idiom from the raw byte stream — the
emit pipeline treats PHA literally and emits a stack write the next
RTS will pop as a return address into a garbage bank. Class fix
(zelda3 alone has 13 instances of the canonical pattern, spread across
banks $05/$06/$09/$1D/$1E — game-agnostic since the idiom is generic
65816 dispatch).

Method:
    1. Walk every cfg entry, decode each function, search its insns
       for the 8-byte byte pattern.
    2. At each hit, compute (table_base = LDA operand, count = how many
       valid-looking entries the table holds).
    3. Append a synthetic entry to cfg.indirect_dispatch with the same
       shape the cfg parser produces, so the existing emit_function
       PHA-dispatch handler picks it up unchanged.

Count inference (conservative):
    - Read 2-byte entries forward from the table base.
    - Reject any entry whose high byte < $80 (LoROM code starts at
      bank-local $8000; valid function pointers can't be in $0000-
      $7FFF for these banks).
    - Reject if the entry is in or past the table itself (a function
      handler can't START inside its own dispatch table).
    - Reject if a subsequent labeled `func` entry in this bank starts
      at or before the current table position + 2 (we've collided
      with the next declared function).
    - Hard cap at 256 entries.

Skip if a cfg directive already authorises this PHA — hand-written
hints win over the auto-router (preserves the existing escape hatch
in case a specific game uses a non-canonical encoding).

Public API:
    detect_and_route(parsed, rom) -> List[FixRecord]
"""

from __future__ import annotations

import sys
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional

_THIS_DIR = Path(__file__).resolve().parent
_RECOMPILER_DIR = _THIS_DIR.parent
for p in (str(_THIS_DIR), str(_RECOMPILER_DIR)):
    if p not in sys.path:
        sys.path.insert(0, p)

# Canonical 8-byte sequence. LL/HH at offsets 1/2 are the LDA operand
# (table base, little-endian). The PHA is at offset 4. We match against
# decoded ROM bytes inside each function body, not the raw image — the
# decoder won't have classified the bytes as code if the byte position
# is unreachable.
_LDA_ABSY_OPCODE = 0xB9
_DEC_A_OPCODE = 0x3A
_PHA_OPCODE = 0x48
_SEP_OPCODE = 0xE2
_SEP30_OPERAND = 0x30
_RTS_OPCODE = 0x60

# Heuristic upper bound for dispatch tables. Largest known ALttP table
# is kSpriteActiveRoutines[243]; we cap at 256 so a malformed match
# can't run away.
_MAX_TABLE_ENTRIES = 256


@dataclass(frozen=True)
class FixRecord:
    """One detected PHA-RTS dispatch site that was auto-routed."""
    bank: int
    pha_pc16: int                 # PC of the PHA byte
    table_pc16: int               # 16-bit base of the dispatch table
    count: int                    # number of dispatch entries detected
    enclosing_func: Optional[str] # name of the cfg func containing this site

    @property
    def site_addr_24(self) -> int:
        return (self.bank << 16) | (self.pha_pc16 & 0xFFFF)


def _rom_offset_lorom(bank: int, addr16: int) -> int:
    """LoROM PC → linear ROM offset. Matches RomPtr in common_rtl.c."""
    return ((bank & 0x7F) << 15) | (addr16 & 0x7FFF)


def _infer_table_count(rom: bytes, bank: int, table_pc16: int,
                       bank_func_starts: list[int]) -> int:
    """Walk the table forward from table_pc16, stopping at the first
    invalid-looking entry. Returns the number of valid entries.

    Validity:
    - high byte of the 16-bit entry must be in $80..$FF (LoROM code
      window of the function's home bank).
    - entry must not point inside the table itself, nor before
      $8000 (RAM mirror, not code).
    - the table cursor must not cross into the start of another
      labeled function in this bank (cfg `func` entries).
    """
    # Pre-compute the next labeled function start strictly greater
    # than table_pc16. The table can't extend at or past that PC.
    next_label = 0x10000
    for s in bank_func_starts:
        if s > table_pc16 and s < next_label:
            next_label = s

    # Tables can include explicit $0000 placeholders for "unused / no
    # handler" slots (verified on ALttP's kGarnish_Funcs at $09:B124 —
    # index 12 is $0000 while indices 13-21 carry real handlers). We
    # allow zero entries to pass through; if such a slot ever dispatches
    # at runtime the decoder's existing dispatch-OOB trap fires.
    # Otherwise stop at the first byte sequence that doesn't look like
    # a bank-local function pointer.
    count = 0
    cur = table_pc16
    while count < _MAX_TABLE_ENTRIES:
        if cur + 2 > next_label:
            break
        off = _rom_offset_lorom(bank, cur)
        if off + 2 > len(rom):
            break
        lo = rom[off]
        hi = rom[off + 1]
        entry = lo | (hi << 8)
        if entry != 0:
            # Non-null entries must look like LoROM code: high byte
            # in $80-$FF and the pointer can't land inside the table.
            if hi < 0x80:
                break
            if table_pc16 <= entry < table_pc16 + (count + 1) * 2:
                break
        count += 1
        cur += 2
    return count


def _scan_function_for_pha_rts(rom: bytes, bank: int,
                               func_start: int, func_end: int,
                               bank_func_starts: list[int]):
    """Scan the ROM byte range [func_start, func_end) inside `bank` for
    the 8-byte PHA-RTS dispatch pattern. Yields (pha_pc16, table_pc16,
    count) per match.

    Raw-byte scan within the cfg-declared function bounds is preferred
    over walking the decoder's graph: a single function may be decoded
    under multiple (entry_m, entry_x) variants, and the PHA-RTS site
    only sits on one of them in zelda3's sprite handlers. Restricting
    to the cfg's start/end bounds is enough to exclude data regions
    (those are either outside any func range or in a `data_region`).
    """
    if func_end <= func_start:
        return
    start_off = _rom_offset_lorom(bank, func_start)
    end_off = _rom_offset_lorom(bank, max(func_start, func_end - 1)) + 1
    if end_off > len(rom):
        end_off = len(rom)
    if start_off >= end_off:
        return
    # Slide an 8-byte window across the function's byte range. The
    # pattern is position-independent inside the function — we don't
    # need to align to instruction boundaries because the bytes are
    # unique enough (B9 LL HH 3A 48 E2 30 60) that they can't appear
    # as a benign mid-instruction substring of any other 65816 mnemonic
    # plus operand sequence.
    span = end_off - start_off - 7
    for i in range(span):
        off = start_off + i
        if (rom[off] == _LDA_ABSY_OPCODE
                and rom[off + 3] == _DEC_A_OPCODE
                and rom[off + 4] == _PHA_OPCODE
                and rom[off + 5] == _SEP_OPCODE
                and rom[off + 6] == _SEP30_OPERAND
                and rom[off + 7] == _RTS_OPCODE):
            table_pc16 = rom[off + 1] | (rom[off + 2] << 8)
            pha_pc16 = (func_start + i + 4) & 0xFFFF
            count = _infer_table_count(rom, bank, table_pc16, bank_func_starts)
            if count == 0:
                continue
            yield pha_pc16, table_pc16, count


def detect_and_route(parsed, rom: bytes) -> List[FixRecord]:
    """Scan every parsed bank cfg for PHA-RTS dispatch sites and
    synthesise `indirect_dispatch` cfg entries for the ones not already
    authorised. Returns the list of detected fixes for reporting.

    Mutates `cfg.indirect_dispatch` in place — adds a dict in the same
    shape the cfg parser produces, so downstream code (v2_regen.py →
    emit_bank → decoder PHA handler) requires no further changes.
    """
    fixes: List[FixRecord] = []
    for bank, _cfg_path, cfg in parsed:
        # Existing hand-written hints win — collect their PHA PCs so
        # we don't double-route. cfg.indirect_dispatch entries store
        # `site_pc16`.
        existing_pcs = {
            d['site_pc16'] & 0xFFFF
            for d in (getattr(cfg, 'indirect_dispatch', None) or [])
        }
        # All labeled function starts in this bank — used by
        # _infer_table_count to clamp table size against the next
        # neighbour function.
        bank_func_starts = sorted(
            e.start & 0xFFFF for e in cfg.entries
        )

        for entry in cfg.entries:
            func_start = entry.start & 0xFFFF
            # Without an explicit `end:`, default to the next labeled
            # function's start (or end-of-bank). Conservative: any
            # PHA-RTS pattern outside this range is owned by a different
            # function and will be detected through its own entry.
            if entry.end is not None:
                func_end = entry.end & 0xFFFF
            else:
                next_start = 0x10000
                for s in bank_func_starts:
                    if s > func_start and s < next_start:
                        next_start = s
                func_end = next_start
            for pha_pc16, table_pc16, count in _scan_function_for_pha_rts(
                    rom, bank, func_start, func_end, bank_func_starts):
                if pha_pc16 in existing_pcs:
                    continue
                cfg.indirect_dispatch.append({
                    'site_pc16': pha_pc16,
                    'count': count,
                    'idx_reg': 'Y',
                    'table_bases': (table_pc16,),
                })
                existing_pcs.add(pha_pc16)
                fixes.append(FixRecord(
                    bank=bank,
                    pha_pc16=pha_pc16,
                    table_pc16=table_pc16,
                    count=count,
                    enclosing_func=entry.name,
                ))
    return fixes


def format_fix_summary(fixes: List[FixRecord]) -> str:
    """Human-readable per-site report."""
    if not fixes:
        return "  no PHA-RTS dispatch sites detected"
    lines = [
        f"  detected {len(fixes)} PHA-RTS dispatch site(s); auto-routed:"
    ]
    for fx in fixes:
        owner = fx.enclosing_func or '???'
        lines.append(
            f"    ${fx.bank:02X}:{fx.pha_pc16:04X}  {owner}  "
            f"-> table ${fx.bank:02X}:{fx.table_pc16:04X}  "
            f"count={fx.count}"
        )
    return "\n".join(lines)
