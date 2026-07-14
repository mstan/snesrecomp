"""snesrecomp.recompiler.v2.decoder

Worklist-driven 65816 decoder keyed by (PC, M, X) entry state.

REPLACES THE V1 DECODE BUG: v1's `decode_func` (recomp.py:52-354) tracks
M/X as linear scalars and stores branch-target mode hints in
`pending_flags: Dict[PC, (m, x)]` with explicit last-writer-wins overwrite
(recomp.py:298-300 comment makes this explicit). When two predecessors
reach the same PC with different (m, x), one is silently dropped and that
PC ends up decoded with the wrong mode — which is invalid for 65816
because variable-length immediate operands (LDA #imm in M=1 vs M=0) are
2 bytes vs 3 bytes, so the dropped mode can corrupt every subsequent
instruction's PC offset.

In v2, every instruction is identified by `DecodeKey(pc, m, x)`. Two
predecessors with different mode states produce two distinct
DecodedInsn records at the same PC — both are preserved. Downstream
(v2 cfg / IR / codegen) treats them as two separate blocks.

The opcode table in `snes65816.py` and the per-instruction
`decode_insn(rom, off, pc, bank, m, x)` helper are reused as-is — they
already correctly compute variable-length immediates *given* an (m, x)
input. The bug was always in the v1 caller, not in `decode_insn`.

Public API:
    decode_function(rom, bank, start, entry_m, entry_x, *, end=None)
        -> FunctionDecodeGraph
"""

from dataclasses import dataclass, field
from typing import Dict, List, Optional, Set, Tuple

import sys
import pathlib

# Allow `from snesrecomp.recompiler.v2 import ...` and standalone test imports.
_THIS_DIR = pathlib.Path(__file__).resolve().parent
_RECOMPILER_DIR = _THIS_DIR.parent
if str(_RECOMPILER_DIR) not in sys.path:
    sys.path.insert(0, str(_RECOMPILER_DIR))

from snes65816 import (  # noqa: E402
    decode_insn, lorom_offset, Insn,
    ABS, INDIR, INDIR_X, LONG, IMM,
)


def addr24(bank: int, pc: int) -> int:
    """Pack bank + 16-bit PC into a 24-bit address (matches Insn.addr)."""
    return ((bank & 0xFF) << 16) | (pc & 0xFFFF)


def _dispatch_target_is_padding(rom: bytes, bank: int, pc16: int,
                                window: int = 16) -> bool:
    """Return True iff the dispatch-table entry's target bytes look like
    unmapped ROM padding (all $FF) or cleared region (all $00).

    Used by the auto-detected dispatch-table reader to terminate a
    table when an entry points into bytes that can't be the start of
    any real handler. SMW's PLA/PLY-indirect-JMP dispatchers
    occasionally have shorter true tables than the auto-detector reads
    (the table's actual count is implicit in the asm code that loads
    the index), and the trailing entries fall on data/padding bytes
    that we then mistakenly auto-promote into phantom functions.

    A target whose first 16 bytes are all $FF is in unmapped ROM
    padding (between real ROM regions, post-end-of-bank, etc.). A
    target whose first 16 bytes are all $00 is similarly suspect.
    Real 65816 code at the target's entry point produces non-uniform
    byte sequences (mix of opcodes + operands).

    NOT a heuristic for code/data classification in general — only
    used to STOP a dispatch table when an obviously-invalid entry is
    encountered. Real code at the target falls through to the
    standard accept path.
    """
    try:
        off = lorom_offset(bank, pc16)
    except AssertionError:
        return True
    if off + window > len(rom):
        return True
    blob = rom[off:off + window]
    if all(b == 0xFF for b in blob):
        return True
    if all(b == 0x00 for b in blob):
        return True
    return False


def _is_abs_indirect_long_jmp(insn) -> bool:
    """True for opcode DC, the 3-byte `JMP [abs]` long-indirect form."""
    return (getattr(insn, 'mnem', '') == 'JMP'
            and getattr(insn, 'opcode', None) == 0xDC)


def _dispatch_kind(insn, table_bases=()) -> str:
    """Return target width for authorized indirect dispatch codegen."""
    return ('long' if (_is_abs_indirect_long_jmp(insn)
                      or getattr(insn, 'length', 3) == 4
                      or len(table_bases or ()) == 3)
            else 'short')


def _autorecover_indirect_dp(rom: bytes, bank: int, func_start: int,
                             site_pc: int, dp_addr: int,
                             insn_length: int,
                             data_regions=None,
                             max_scan_insns: int = 256
                             ) -> Optional[Tuple[Tuple[int, ...], str]]:
    """For a `JMP ($<dp>)` (length 3, 16-bit indirect) or `JML [<dp>]`
    (length 3, opcode DC, 24-bit indirect at abs <dp>) at `site_pc`:
    walk the function from `func_start` to `site_pc-1`, accumulating
    `LDA <ABS_X table>,X / STA $<dp>`-style pair sequences that write
    the DP pointer immediately before the dispatch. Returns
    `(table_bases, idx_reg)` if a pattern matched, else None.

    Pattern shapes recognised:
      A. JMP indirect, M=1 split-byte form:
           LDA <tbl_lo>,X ; STA $<dp>
           LDA <tbl_hi>,X ; STA $<dp+1>
         → return ((tbl_lo, tbl_hi), 'X')
      B. JML indirect, M=1 split-byte form:
           LDA <tbl_lo>,X ; STA $<dp>
           LDA <tbl_hi>,X ; STA $<dp+1>
           LDA <tbl_bk>,X ; STA $<dp+2>
         → return ((tbl_lo, tbl_hi, tbl_bk), 'X')
      C. M=0 single-word form:
           LDA <tbl>,X ; STA $<dp>   (16-bit LDA/STA covers both bytes)
         → return ((tbl,), 'X')

    Index register: derived from the LDA's addressing mode (`,X` or `,Y`).
    Both must use the same index. Mixed forms are rejected.

    The walk is linear from func_start. Branches and intervening writes
    to the DP slots are tolerated as long as the LDA/STA pair that
    "wins" is the most recent one before the dispatch. SEP/REP state is
    tracked so M is known when the JMP fires.
    """
    from snes65816 import (decode_insn, lorom_offset, ABS_X, LONG_X,
                            ABS_Y, DP)
    # Most-recent winners per DP slot (offset 0, 1, 2 from dp_addr base).
    # Each is (table_base, table_mode, idx_reg).
    winners: dict = {}
    pc = func_start & 0xFFFF
    m_state = 1
    x_state = 1
    last_lda_table: Optional[Tuple[int, int, str]] = None
    scanned = 0
    while pc < site_pc and scanned < max_scan_insns:
        try:
            off = lorom_offset(bank, pc)
        except AssertionError:
            return None
        if off >= len(rom):
            return None
        try:
            insn = decode_insn(rom, off, pc=pc, bank=bank,
                               m=m_state, x=x_state)
        except Exception:
            return None
        if insn is None:
            return None
        mnem = insn.mnem
        # Track M/X state across REP/SEP — table-base recovery is the
        # same regardless of M, but instruction LENGTHS depend on it.
        if mnem == 'REP':
            if insn.operand & 0x20:
                m_state = 0
            if insn.operand & 0x10:
                x_state = 0
        elif mnem == 'SEP':
            if insn.operand & 0x20:
                m_state = 1
            if insn.operand & 0x10:
                x_state = 1
        # Capture the most-recent LDA <ABS/LONG, X/Y> — these are the
        # candidate sources for the next STA to a DP slot.
        if mnem == 'LDA' and insn.mode in (ABS_X, LONG_X):
            last_lda_table = (insn.operand & 0xFFFF, insn.mode, 'X')
        elif mnem == 'LDA' and insn.mode == ABS_Y:
            last_lda_table = (insn.operand & 0xFFFF, insn.mode, 'Y')
        elif mnem == 'STA' and insn.mode == DP:
            slot = (insn.operand & 0xFFFF) - (dp_addr & 0xFFFF)
            if 0 <= slot <= 2 and last_lda_table is not None:
                # Pair the LDA we just saw with this STA — it
                # writes one byte of the dispatch pointer.
                winners[slot] = last_lda_table
                # Don't reuse the same LDA for another slot.
                last_lda_table = None
        # Any other write to one of the DP slots WITHOUT a preceding
        # paired LDA invalidates the pattern (someone else clobbers it).
        elif mnem == 'STA' or mnem == 'STZ':
            slot = (insn.operand & 0xFFFF) - (dp_addr & 0xFFFF)
            if 0 <= slot <= 2:
                # Allow STZ + STA without LDA pairing only if it's
                # writing zero (defensive). Drop the slot.
                winners.pop(slot, None)
        # Any LDA to a non-table mode — clear the candidate.
        elif mnem == 'LDA':
            last_lda_table = None
        scanned += 1
        pc = (pc + insn.length) & 0xFFFF

    if not winners:
        return None

    # Validate winners: same idx_reg across all collected slots.
    idx_regs = set(w[2] for w in winners.values())
    if len(idx_regs) != 1:
        return None
    idx_reg = next(iter(idx_regs))

    # Resolve to ordered tuple of table bases. Need consecutive slots
    # starting at slot 0. For JML (insn_length == 3, opcode DC → 24-bit
    # indirect) expect 3 slots; for JMP (16-bit indirect) expect 1 or 2.
    needed_slots = 3 if (insn_length == 3 and m_state == 1
                          # Heuristic: JML form needs 3 (opcode DC).
                          # Caller will sanity-check via opcode anyway.
                          and 2 in winners) else (2 if 1 in winners else 1)
    table_bases: List[int] = []
    for s in range(needed_slots):
        if s not in winners:
            return None
        table_bases.append(winners[s][0])
    return (tuple(table_bases), idx_reg)


def _autorecover_dp_table_count(rom: bytes, bank: int,
                                table_bases: Tuple[int, ...],
                                data_regions=None,
                                max_entries: int = 256) -> Optional[int]:
    """Walk the parallel byte-tables for a DP-pointer dispatch and
    return the count of valid entries before the first invalid one.

    table_bases: 1, 2 or 3 16-bit table bases in the dispatching insn's
    bank. Each table holds one byte per dispatch index. The composed
    target is:
        len==1: pointer = (bank << 16) | (rom[base[0]+i] but how is
                  this a 16-bit ptr from 1 byte? Skipped — len==1 is
                  not a valid composition; returns 0 in that case.)
        len==2: pointer = (bank << 16) | (hi << 8) | lo
        len==3: pointer = (bank << 16) | (hi << 8) | lo  — wait, with
                  bank table: pointer = (bk << 16) | (hi << 8) | lo

    For each i in 0..max-1, check the composed pointer is valid:
      - target's bank in [00..FF] (always true, defensive)
      - target's 16-bit pc in [$8000..$FFFF]
      - target bytes don't look like padding
      - target not in any data_region
    Stop at first invalid; return how many were valid.
    """
    if not table_bases:
        return None
    if len(table_bases) == 1:
        # M=0 single-word form: each table entry is a 2-byte ptr at
        # `base + 2*i` in the dispatcher's bank. Treat the table as
        # a contiguous 16-bit-entry array; walk until invalid.
        base = table_bases[0] & 0xFFFF
        count = 0
        for i in range(max_entries):
            tbl_pc = (base + 2 * i) & 0xFFFF
            if tbl_pc + 1 > 0xFFFF:
                break
            try:
                off = lorom_offset(bank, tbl_pc)
            except AssertionError:
                break
            if off + 1 >= len(rom):
                break
            addr16 = rom[off] | (rom[off + 1] << 8)
            if addr16 == 0:
                break
            if addr16 < 0x8000:
                break
            if _addr_in_data_regions(data_regions, bank, addr16):
                break
            if _dispatch_target_is_padding(rom, bank, addr16):
                break
            count += 1
        return count if count > 0 else None
    lo_base = table_bases[0] & 0xFFFF
    hi_base = table_bases[1] & 0xFFFF
    bk_base = table_bases[2] & 0xFFFF if len(table_bases) >= 3 else None
    count = 0
    for i in range(max_entries):
        try:
            lo_off = lorom_offset(bank, (lo_base + i) & 0xFFFF)
            hi_off = lorom_offset(bank, (hi_base + i) & 0xFFFF)
        except AssertionError:
            break
        if max(lo_off, hi_off) >= len(rom):
            break
        lo = rom[lo_off]
        hi = rom[hi_off]
        if bk_base is not None:
            try:
                bk_off = lorom_offset(bank, (bk_base + i) & 0xFFFF)
            except AssertionError:
                break
            if bk_off >= len(rom):
                break
            eb = rom[bk_off]
        else:
            eb = bank
        addr16 = (hi << 8) | lo
        if addr16 == 0:
            # Single null tolerated; two consecutive = stop.
            # Simpler: stop on first null. Real handlers don't sit at $0000.
            break
        if addr16 < 0x8000:
            break
        if _addr_in_data_regions(data_regions, eb, addr16):
            break
        if _dispatch_target_is_padding(rom, eb, addr16):
            break
        count += 1
    return count if count > 0 else None


def _autorecover_indirect_xtable(rom: bytes, bank: int, insn,
                                 data_regions=None,
                                 max_entries: int = 256,
                                 func_start: Optional[int] = None) -> Optional[List[int]]:
    """Walk the dispatch table for a `JMP (abs,X)` / `JML (abs,X)` /
    `JSR (abs,X)` at `insn`. Returns a list of 24-bit target PCs, or
    None if the very first entry already looks invalid (no table at
    this site).

    Termination rules (in order):
      1. table address would cross the bank boundary ($FFFF)
      2. table would reach the dispatch site's own function start —
         the table-precedes-dispatcher layout's hard code boundary
      3. ROM-offset goes off the image
      4. raw target value is null ($0000) AND the next entry is also
         null — pad detected. Single $0000 is preserved as a null-entry
         (some jump tables intentionally have a no-op slot)
      5. raw target value is < $8000 (not LoROM code space)
      6. target points into a cfg `data_region` for the resolved bank
      7. target bytes look like padding ($FF / $00 fill) per
         `_dispatch_target_is_padding`
      8. max_entries hit (defensive)

    `func_start` (16-bit) is the entry PC of the function containing the
    dispatch site. The dominant 65816 layout puts the table immediately
    BEFORE its dispatcher (`<table bytes> ; <dispatcher code>`), so when
    `base < func_start` the table cannot extend at/beyond `func_start`
    without overrunning into the dispatcher's own opcodes. Without this
    bound the walk reads the dispatcher bytes (e.g. `A5 B0 0A AA …`) as
    bogus entries whenever none of the real handlers happens to sit at
    the table's true end.

    Entry size: 2 for JMP (length 3) — INDIR_X target stays in current
    bank. 3 for JML (length 4) — INDIR_X target is a 24-bit pointer.
    """
    base = insn.operand & 0xFFFF
    entry_size = 3 if _dispatch_kind(insn) == 'long' else 2
    entries: List[int] = []
    tbl_pc = base
    nulls_in_a_row = 0
    # Hard code boundary for the table-precedes-dispatcher layout: the
    # table ends where the dispatcher's own function begins.
    code_boundary = None
    if func_start is not None and base < (func_start & 0xFFFF):
        code_boundary = func_start & 0xFFFF
    # In-bank handler PCs we've already accepted. Used as an upper bound
    # on the table — once tbl_pc would meet or cross any accepted
    # in-bank handler, we've walked into that handler's code and any
    # further "entry" is mis-decoded handler bytes. Catches the class
    # where the table sits immediately before its first handler (most
    # common 65816 layout: `JSR ($base,X) ; JMP <after> ; <table bytes> ;
    # <handler 0 code>`). Without this check, the walker reads the first
    # 2-3 bytes of handler 0 as a bogus entry, which then gets auto-
    # promoted to a phony function entry — manifests downstream as a
    # cross-fn goto trap to garbage (MMX bank 01 $848E from the
    # $01:D11B dispatch + bank 02 $8F20 from the $02:CFD4 dispatch).
    #
    # Only handlers AT OR ABOVE the table base count: the walk grows
    # upward from `base`, so a target that lives BELOW the table (a
    # handler elsewhere in the bank, e.g. Zelda Module11's submodule-3
    # handler at $8D10 sitting below its table at $9AED) can never be
    # "code the table ran into" and must not terminate the walk. Before
    # this filter, accepting such an entry falsely capped the table at
    # the next slot (Module11 truncated 6→4 entries → submodule-4
    # dispatch_oob trap on walking up into a dungeon falling entrance).
    inbank_handler_pcs: List[int] = []
    while len(entries) < max_entries:
        if tbl_pc + entry_size - 1 > 0xFFFF:
            break
        # Stop at the dispatcher's own function start (table-precedes-
        # dispatcher layout): no entry can begin at/after it.
        if code_boundary is not None and tbl_pc >= code_boundary:
            break
        # Stop if the next entry's read range would overlap any already-
        # accepted in-bank handler at/above the table base. Equivalent
        # to: table_top >= min(handler_pc for handler_pc >= base).
        if any(tbl_pc >= h for h in inbank_handler_pcs):
            break
        try:
            off = lorom_offset(bank, tbl_pc & 0xFFFF)
        except AssertionError:
            break
        if off + entry_size - 1 >= len(rom):
            break
        addr16 = rom[off] | (rom[off + 1] << 8)
        if entry_size == 3:
            eb = rom[off + 2]
            full = (eb << 16) | addr16
        else:
            eb = bank
            full = (bank << 16) | addr16
        if addr16 == 0 and (entry_size == 2 or eb == 0):
            # Null entry. Tolerate one in a row (some tables leave a
            # slot blank intentionally); two consecutive nulls = pad.
            nulls_in_a_row += 1
            if nulls_in_a_row >= 2:
                # Drop the trailing null we already appended (one too
                # many).
                if entries and entries[-1] == 0:
                    entries.pop()
                break
            entries.append(0)
            tbl_pc += entry_size
            continue
        nulls_in_a_row = 0
        if addr16 < 0x8000:
            break
        if entry_size == 3 and (eb < 0x00 or eb > 0xFF):  # defensive
            break
        if entry_size == 3 and addr16 < 0x8000:
            break
        if _addr_in_data_regions(data_regions, eb, addr16):
            break
        if _dispatch_target_is_padding(rom, eb, addr16):
            break
        entries.append(full)
        # Track in-bank handler PCs so the next iteration's overlap
        # check can stop walking once tbl_pc would cross into one. Only
        # handlers at/above the table base are candidates for "code the
        # upward-growing table ran into"; targets below `base` live
        # elsewhere in the bank and must not bound the walk.
        if eb == bank and base <= addr16 <= 0xFFFF:
            inbank_handler_pcs.append(addr16)
        tbl_pc += entry_size
    return entries if entries else None


def _autorecover_local_stride_runway(rom: bytes, bank: int, func_start: int,
                                     site_pc: int, insn,
                                     *,
                                     end: Optional[int] = None,
                                     data_regions=None,
                                     max_entries: int = 4096
                                     ) -> Optional[List[int]]:
    """Recover same-function Duff-runway computed jumps.

    Super Metroid clears OAM with a computed `JMP ($dp)` into the middle
    of a same-function run of `STA abs` instructions:

        LSR A
        STA $dp
        LSR A
        ADC $dp
        CLC
        ADC #runway_base
        STA $dp
        LDA #imm16
        SEP #$30
        JMP ($dp)

    The target pointer is `runway_base + 3 * logical_index`, so every valid
    target is an interior label in the current generated C function. These
    entries must be decoded as local successors, not auto-promoted as
    standalone callable functions.
    """
    if not (insn.mnem == 'JMP' and insn.mode == INDIR and insn.length == 3):
        return None
    dp_addr = insn.operand & 0xFFFF
    if dp_addr > 0x00FF:
        return None

    def read8(pc16: int) -> Optional[int]:
        if not (0x8000 <= pc16 <= 0xFFFF):
            return None
        try:
            off = lorom_offset(bank, pc16)
        except AssertionError:
            return None
        if off >= len(rom):
            return None
        return rom[off]

    site_pc &= 0xFFFF
    p = site_pc - 17
    if p < 0x8000:
        return None

    dp = dp_addr & 0xFF
    expected = (
        (0, 0x4A),        # LSR A
        (1, 0x85),        # STA dp
        (2, dp),
        (3, 0x4A),        # LSR A
        (4, 0x65),        # ADC dp
        (5, dp),
        (6, 0x18),        # CLC
        (7, 0x69),        # ADC #imm16
        (10, 0x85),       # STA dp
        (11, dp),
        (12, 0xA9),       # LDA #imm16
        (15, 0xE2),       # SEP #$30
        (16, 0x30),
        (17, 0x6C),       # JMP ($dp)
        (18, dp),
        (19, 0x00),
    )
    for rel, byte in expected:
        got = read8(p + rel)
        if got != byte:
            return None

    base_lo = read8(p + 8)
    base_hi = read8(p + 9)
    if base_lo is None or base_hi is None:
        return None
    base = base_lo | (base_hi << 8)
    if not (0x8000 <= base <= 0xFFFF):
        return None
    start16 = func_start & 0xFFFF
    if base < start16:
        return None
    if end is not None and base >= (end & 0xFFFF):
        return None
    if _addr_in_data_regions(data_regions, bank, base):
        return None
    if _dispatch_target_is_padding(rom, bank, base):
        return None

    entries: List[int] = []
    first_store_addr: Optional[int] = None
    for i in range(max_entries):
        target_pc = base + i * 3
        if target_pc + 2 > 0xFFFF:
            break
        if end is not None and target_pc >= (end & 0xFFFF):
            break
        if _addr_in_data_regions(data_regions, bank, target_pc):
            break
        op = read8(target_pc)
        lo = read8(target_pc + 1)
        hi = read8(target_pc + 2)
        if op != 0x8D or lo is None or hi is None:
            break
        store_addr = lo | (hi << 8)
        if first_store_addr is None:
            first_store_addr = store_addr
        elif store_addr != ((first_store_addr + i * 4) & 0xFFFF):
            break
        entries.append((bank << 16) | target_pc)

    # Require several consecutive runway slots. A one- or two-instruction
    # match is more likely ordinary code than a computed Duff runway.
    if len(entries) < 4:
        return None
    return entries


def _addr_in_data_regions(data_regions, bank: int, pc16: int) -> bool:
    """Return True iff (bank, pc16) is inside any cfg-declared
    `data_region <bank> <start> <end>` range.

    cfg `data_region` directives encode a real ROM fact: this byte
    range is a data table, not executable code. Used by the dispatch-
    table reader to halt at entries whose targets land inside data,
    and by the auto-promote pass to refuse synthesizing function
    entries inside data ranges. The classic case is a JSL dispatcher
    whose table overruns into a sibling data table — without the
    cfg fact the decoder can't tell those bytes apart from real
    handlers.

    `data_regions` is the list[tuple[int, int, int]] of (bank, start,
    end_exclusive) tuples produced by cfg_loader. None / empty list
    is a no-op (returns False).
    """
    if not data_regions:
        return False
    pc16 &= 0xFFFF
    bank &= 0xFF
    for (b, s, e) in data_regions:
        if (b & 0xFF) != bank:
            continue
        if (s & 0xFFFF) <= pc16 < (e & 0xFFFF):
            return True
    return False


@dataclass(frozen=True)
class DecodeKey:
    """Identifies a decoded instruction by 24-bit address + entry M/X +
    PHP/PLP stack history.

    Two DecodeKeys are equal iff (pc, m, x, p_stack) all match. Same `pc`
    with different (m, x, p_stack) is multiple distinct keys → multiple
    distinct decoded instances in the graph.

    `p_stack` tracks the LIFO (m, x) snapshots PHP'd within the current
    function body but not yet PLP'd. Each PHP pushes the current (m, x)
    onto this stack; each PLP pops the top entry and RESTORES (m, x) to
    that popped value. Without this tracking, the canonical SMW idiom
    `PHX ; PHY ; PHP ; SEP #$30 ; … ; PLP ; PLY ; PLX ; RTS` (used by
    UpdateSaveBuffer, NMI handlers, and many SEP-bracketed helpers)
    de-syncs static-width pinning at the PLP-restored PLX/PLY: the
    decoder otherwise stays at the post-SEP (m=1, x=1) state through
    PLP, producing 1-byte pops where the runtime expects 2-byte (entry)
    width. PHP/PLP tracking lets the decoder revert to the saved state
    at PLP so push and pull widths match across the bracket.

    Bounded at depth 8; deeper PHP nesting is treated as an unmodeled
    runtime-only state (PHP becomes a no-op for p_stack growth).
    """
    pc: int   # 24-bit ((bank << 16) | local_pc)
    m: int    # entry M flag, 0 or 1
    x: int    # entry X flag, 0 or 1
    p_stack: Tuple[Tuple[int, int], ...] = ()  # PHP-pushed (m, x) LIFO


@dataclass
class DecodedInsn:
    """One instruction decoded at one specific (pc, m, x) entry state."""
    key: DecodeKey
    insn: Insn               # the underlying snes65816.Insn (m_flag/x_flag set to entry m/x)
    successors: List[DecodeKey]


@dataclass
class SuppressedIndirectCall:
    """Bookkeeping entry for a JSR (abs,X) site whose fall-through edge
    was severed because cfg has no `indirect_call_table` directive
    authorising it.

    cfg-required-dispatch-or-kill rule (2026-05-03): the v2 decoder
    refuses to follow the fall-through of an indirect JSR (a,X) when
    cfg hasn't declared a static dispatch table for it. The insn is
    still placed in the graph (so predecessors' successor edges
    resolve), but with successors=[] — that severs the post-JSR
    decode chain so phantom M=0 paths through SMW's SMC-dispatch byte
    sequences don't pollute downstream codegen.

    Each suppressed site is recorded here for the build report. Any
    reach of `site_pc24` at runtime is caught by the always-armed
    phantom-PC trap (runner/src/cpu_trace.c).
    """
    site_pc24: int
    table_base: int
    function_entry_pc24: int
    entry_m: int
    entry_x: int


@dataclass
class DispatchTargetSuppressed:
    """Bookkeeping for a dispatch-table entry the decoder REFUSED to
    accept because the target lands inside a cfg `data_region` (an
    explicit ROM-structure fact saying "these bytes are data, not
    code"). The dispatch table is truncated at this entry; the
    target never becomes a callable handler. Recorded so the build
    report can list every suppression — never silent.
    """
    site_pc24: int       # PC of the dispatcher JSL/JML
    target_pc24: int     # 24-bit address of the rejected entry
    reason: str          # 'data_region' (extensible)
    table_index: int     # 0-based index of the entry that triggered the stop


@dataclass
class UnresolvedIndirect:
    """Bookkeeping entry for an indirect JMP/JML/JSR whose static target
    list the decoder could not recover. Either:
      - auto-recovery didn't match a known idiom at the site, AND
      - no cfg `indirect_dispatch` directive authorised the site.
    v2_regen hard-fails on any non-empty list — there is no stub
    fallback. Authoring an `indirect_dispatch <site> <count> idx:<reg>
    [tables:...]` line in the cfg is the resolution path; recompiler-
    level auto-recovery extensions are the more-complete path.
    """
    site_pc24: int
    mnem: str                # 'JMP' | 'JML' | 'JSR'
    mode: int                # raw addressing mode (snes65816 module)
    operand: int             # raw operand from the insn
    function_entry_pc24: int
    entry_m: int
    entry_x: int


@dataclass
class ConstZFold:
    """Bookkeeping entry for a BEQ/BNE rewritten to an unconditional Goto
    by `_apply_constant_z_fold`. Recorded for the build report so each
    fold is visible/auditable rather than silently absorbed.
    """
    branch_pc24: int          # PC of the BEQ/BNE
    prev_pc24: int            # PC of the preceding LDA/LDX/LDY #imm
    branch_mnem: str          # 'BEQ' | 'BNE'
    prev_mnem: str            # 'LDA' | 'LDX' | 'LDY'
    prev_imm: int             # masked immediate value used for Z
    width_bits: int           # 8 or 16 (op width at the load)
    z_value: int              # 0 or 1
    taken_kind: str           # 'jump' (live edge is the explicit target)
                              # or 'fall' (live edge is fall-through PC)
    live_pc24: int            # surviving successor's 24-bit PC
    dead_pc24: int            # pruned successor's 24-bit PC
    func_entry_pc24: int      # decode_function's entry PC for context
    entry_m: int
    entry_x: int


@dataclass
class FunctionDecodeGraph:
    """Output of `decode_function` for one function entry.

    Attributes:
        entry: the DecodeKey we started at.
        insns: dict keyed by DecodeKey. Two entries may share `key.pc`
            iff they have different `key.m` or `key.x` — that means the
            same PC was decoded twice, once per reaching mode-state, and
            both are preserved. (This is the central correctness fix.)
        suppressed_indirect_calls: list of JSR (abs,X) sites whose
            fall-through edge was severed because cfg has no
            `indirect_call_table` authorisation. See class
            SuppressedIndirectCall above.
        const_z_folds: list of BEQ/BNE rewrites by the constant-Z fold
            post-pass. Each entry records the original branch + the
            statically-proven Z + the surviving and pruned edges.
    """
    entry: DecodeKey
    insns: Dict[DecodeKey, DecodedInsn] = field(default_factory=dict)
    suppressed_indirect_calls: List[SuppressedIndirectCall] = field(default_factory=list)
    const_z_folds: List[ConstZFold] = field(default_factory=list)
    dispatch_targets_suppressed: List[DispatchTargetSuppressed] = field(default_factory=list)
    # Indirect JMP / JML / JSR sites that the decoder COULD NOT resolve
    # statically: no auto-recovery pattern matched AND no cfg
    # `indirect_dispatch` directive authorised them. v2_regen treats any
    # non-empty list as a hard build failure (no-stub policy).
    unresolved_indirects: List['UnresolvedIndirect'] = field(default_factory=list)
    # Direct calls whose architectural return M/X state was not yet proven.
    # LLE-first analysis asks the decoder to stop at these sites instead of
    # guessing that the callee preserves the caller's widths.  Each tuple is
    # (site_pc24, target_pc24, entry_m, entry_x).
    unknown_callee_exit_sites: List[Tuple[int, int, int, int]] = field(
        default_factory=list)

    def keys_at_pc(self, pc24: int) -> List[DecodeKey]:
        """Return all DecodeKeys with this 24-bit PC (across entry mode states)."""
        return [k for k in self.insns if k.pc == pc24]

    def insns_at_pc(self, pc24: int) -> List[DecodedInsn]:
        return [self.insns[k] for k in self.keys_at_pc(pc24)]


# Mnemonics with no fall-through successor.
_TERMINATORS = frozenset({'RTS', 'RTL', 'RTI', 'STP', 'WAI', 'BRK'})

# Mnemonics with two successors: fall-through AND taken-branch target.
_COND_BRANCHES = frozenset({'BPL', 'BMI', 'BVC', 'BVS', 'BCC', 'BCS', 'BNE', 'BEQ'})


def _resolve_indirect_dispatch_targets(rom: bytes, bank: int, insn,
                                       auth: dict) -> Optional[List[int]]:
    """Read N dispatch targets from ROM per the cfg `indirect_dispatch`
    directive. Returns a list of 24-bit PC targets, or None if the
    table-base layout doesn't fit the addressing mode of `insn`.

    auth shape: {
      'count':       int N,
      'idx_reg':     'X' | 'Y',
      'table_bases': tuple of 0..3 16-bit bases.
    }

    Resolution rules:
      ()         — table base is `insn.operand` (JSR/JMP/JML (abs,X) form,
                   or JMP (abs)/JML [abs] with operand as table addr).
                   Entry size = 3 for JML/JSL and opcode DC (`JMP [abs]`),
                   else 2.
      (lo,)      — single static table at `lo`. Entry size from `insn`.
      (lo, hi)   — 2 parallel byte-tables forming a 16-bit pointer per
                   index. Target = (bank << 16) | (rom[hi+i] << 8) | rom[lo+i].
      (lo, hi, bk) — 3 parallel byte-tables forming a 24-bit pointer per
                   index. Target = (rom[bk+i] << 16) | (rom[hi+i] << 8) | rom[lo+i].
                   This is the Module_MainRouting / JML [DP] form.

    All table reads are in the dispatching insn's bank (`bank`). LoROM
    range check + ROM-length check applied per entry; out-of-range
    returns None (cfg/ROM mismatch).
    """
    count = int(auth['count'])
    bases = auth.get('table_bases') or ()

    # Explicit target list: no ROM table to walk. Used by pointer-sourced
    # CALL cfg (`ptrcall`) and by local computed-goto runway recovery.
    if auth.get('targets'):
        entries: List[int] = []
        for t in auth['targets']:
            tv = int(t)
            if tv > 0xFFFFFF:
                return None
            entries.append(tv if tv > 0xFFFF else ((bank << 16) | (tv & 0xFFFF)))
        if len(entries) != count:
            return None
        return entries

    if len(bases) >= 2:
        # Parallel byte-tables. Walk i = 0..count-1, read one byte from
        # each table, compose the target.
        lo_base = bases[0] & 0xFFFF
        hi_base = bases[1] & 0xFFFF
        bk_base = bases[2] & 0xFFFF if len(bases) == 3 else None
        entries: List[int] = []
        for i in range(count):
            try:
                lo_off = lorom_offset(bank, (lo_base + i) & 0xFFFF)
                hi_off = lorom_offset(bank, (hi_base + i) & 0xFFFF)
            except AssertionError:
                return None
            if max(lo_off, hi_off) >= len(rom):
                return None
            lo = rom[lo_off]
            hi = rom[hi_off]
            if bk_base is not None:
                try:
                    bk_off = lorom_offset(bank, (bk_base + i) & 0xFFFF)
                except AssertionError:
                    return None
                if bk_off >= len(rom):
                    return None
                eb = rom[bk_off]
                entries.append((eb << 16) | (hi << 8) | lo)
            else:
                entries.append((bank << 16) | (hi << 8) | lo)
        return entries

    # Single-table form. Base is operand (bases=()) or bases[0] (bases=(lo,)).
    if bases:
        base = bases[0] & 0xFFFF
    else:
        base = insn.operand & 0xFFFF
    # Entry size: 3 for JML/JSL and for opcode DC (`JMP [abs]`, a
    # 3-byte instruction that loads a 24-bit target); otherwise 2.
    entry_size = 3 if _dispatch_kind(insn) == 'long' else 2
    entries: List[int] = []
    tbl_pc = base
    for _i in range(count):
        if tbl_pc + entry_size - 1 > 0xFFFF:
            return None
        try:
            off = lorom_offset(bank, tbl_pc & 0xFFFF)
        except AssertionError:
            return None
        if off + entry_size - 1 >= len(rom):
            return None
        addr16 = rom[off] | (rom[off + 1] << 8)
        if entry_size == 3:
            eb = rom[off + 2]
            entries.append((eb << 16) | addr16)
        else:
            entries.append((bank << 16) | addr16)
        tbl_pc += entry_size
    return entries


# Maximum PHP nesting depth tracked by p_stack. Bounded to keep the
# state space finite — beyond this depth, additional PHPs are no-ops for
# decoder state (any PLP at that depth conservatively keeps the current
# (m, x)). SMW typical code uses depth 0–1, sometimes 2; 8 is safe.
_PHP_STACK_MAX_DEPTH = 8


def post_state(insn: Insn, in_m: int, in_x: int,
               in_p_stack: Tuple[Tuple[int, int], ...] = ()
               ) -> Tuple[int, int, Tuple[Tuple[int, int], ...]]:
    """Compute (m, x, p_stack) AFTER executing `insn`, given entry state.

    REP/SEP clear/set M and X bits independently per the operand bitmask;
    p_stack is unchanged (REP/SEP don't push P).

    PHP pushes the current (m, x) onto p_stack; (m, x) themselves are
    unchanged (PHP only pushes P, doesn't modify the flag bits). At PLP
    later, this snapshot is restored.

    PLP pops the top of p_stack and restores (m, x) to that snapshot. If
    p_stack is empty (unbalanced PLP — caller pushed P, or a coding
    error), keep (m, x) at the current state (conservative).

    XCE, RTI, and other M/X-affecting ops not modeled here — they keep
    the current state. PLP via this path correctly handles the
    PHP/PLP-balanced common case.
    """
    mnem = insn.mnem
    if mnem == 'REP':
        m = 0 if (insn.operand & 0x20) else in_m
        x = 0 if (insn.operand & 0x10) else in_x
        return m, x, in_p_stack
    if mnem == 'SEP':
        m = 1 if (insn.operand & 0x20) else in_m
        x = 1 if (insn.operand & 0x10) else in_x
        return m, x, in_p_stack
    if mnem == 'PHP':
        if len(in_p_stack) < _PHP_STACK_MAX_DEPTH:
            return in_m, in_x, in_p_stack + ((in_m, in_x),)
        # Stack overflow — keep current state, drop the push silently.
        # Beyond depth 8 we lose tracking but don't pollute state.
        return in_m, in_x, in_p_stack
    if mnem == 'PLP':
        if in_p_stack:
            popped_m, popped_x = in_p_stack[-1]
            return popped_m, popped_x, in_p_stack[:-1]
        # PLP with empty p_stack — caller pushed P before JSR, or
        # unbalanced. Keep current state.
        return in_m, in_x, in_p_stack
    return in_m, in_x, in_p_stack


def post_mx(insn: Insn, in_m: int, in_x: int) -> Tuple[int, int]:
    """Back-compat shim: returns just (m, x) without p_stack tracking.

    Callers that don't thread p_stack will lose PHP/PLP-bracketed
    correctness. New code should use post_state() directly. Kept for
    any external/test code that imports post_mx.
    """
    m, x, _ = post_state(insn, in_m, in_x, ())
    return m, x


def _successors(insn: Insn, key: DecodeKey, bank: int,
                callee_exit_mx: Optional[Dict] = None,
                callee_exit_mx_modes: Optional[Dict] = None,
                stop_on_unknown_callee_exit: bool = False,
                unknown_callee_exit_sites: Optional[List] = None,
                ) -> List[DecodeKey]:
    """Compute successor DecodeKeys for one decoded instruction.

    Returns plain DecodeKey list (kind-agnostic) for callers that only
    need successors. See `_labeled_successors` for the (key, kind)
    variant used by `decode_function`'s end: gating logic.
    """
    return [k for (k, _kind) in
            _labeled_successors(insn, key, bank,
                                callee_exit_mx=callee_exit_mx,
                                callee_exit_mx_modes=callee_exit_mx_modes,
                                stop_on_unknown_callee_exit=(
                                    stop_on_unknown_callee_exit),
                                unknown_callee_exit_sites=(
                                    unknown_callee_exit_sites))]


def _labeled_successors(insn: Insn, key: DecodeKey, bank: int,
                        callee_exit_mx: Optional[Dict] = None,
                        callee_exit_mx_modes: Optional[Dict] = None,
                        rom: Optional[bytes] = None,
                        inline_arg_map: Optional[Dict[int, int]] = None,
                        stop_on_unknown_callee_exit: bool = False,
                        unknown_callee_exit_sites: Optional[List] = None):
    """Compute (DecodeKey, edge_kind) tuples for one decoded instruction.

    `edge_kind` is one of:
        'jump'        — control-flow edge whose TARGET is named explicitly
                        in the insn (BRA/BRL/cond-branch-taken/JMP-ABS).
                        These edges may cross the cfg-declared end:
                        boundary because the asm explicitly transfers
                        there — the original routine's lifetime extends
                        across them, even though `end:` says the next
                        cfg function starts at that PC.
        'fall'        — natural fall-through to the next instruction
                        (linear, JSR/JSL-after-call, cond-branch-not-
                        taken). These edges respect end:; falling
                        through past end: would pull the next function's
                        body into this one and is forbidden.
        terminator    -> [] (no successors)
        JMP INDIR/(X) -> [] (table-driven; caller's job)
        JMP LONG/JML  -> [] (cross-bank; caller's job)

    The distinction matters for the inline-cross-fn-blocks model
    (control-flow correctness fix, 2026-05-02): a BRA into a label past
    end: must IMPORT that label's blocks into the current function so
    PHB/PLB pairs and other stack-lifetime invariants stay matched
    within one C function scope. Treating arbitrary jump targets as new
    C functions (the prior auto-promote behavior) split asm routines
    across multiple C bodies and stranded their PHBs without their
    matching PLBs — root cause of DB=$C0 at dispatch entry.
    """
    post_m, post_x, post_p_stack = post_state(insn, key.m, key.x, key.p_stack)
    pc = insn.addr & 0xFFFF
    next_pc = (pc + insn.length) & 0xFFFF

    mnem = insn.mnem

    if mnem in _TERMINATORS:
        return []

    if mnem in ('BRA', 'BRL'):
        return [(DecodeKey(addr24(bank, insn.operand), post_m, post_x, post_p_stack), 'jump')]

    if mnem in _COND_BRANCHES:
        return [
            (DecodeKey(addr24(bank, next_pc), post_m, post_x, post_p_stack), 'fall'),
            (DecodeKey(addr24(bank, insn.operand), post_m, post_x, post_p_stack), 'jump'),
        ]

    if mnem == 'JMP':
        if insn.mode == ABS:
            return [(DecodeKey(addr24(bank, insn.operand), post_m, post_x, post_p_stack), 'jump')]
        # INDIR / INDIR_X (table-dispatch) and LONG (cross-bank) — no
        # static successors at this layer.
        return []

    # Long-jump (JML) is decoded as JMP+LONG above; JSL is its own mnem.
    # Both are cross-routine calls; only the fall-through (return site)
    # is decoded into THIS function. The callee is a separate cfg entry.
    #
    # If `callee_exit_mx` provides this callee's exit (m, x) under the
    # entry variant we're calling with, use it for the fall-through key.
    # Without that, we'd assume m/x are preserved across the JSR — wrong
    # whenever the callee has an internal SEP/REP that doesn't restore
    # before returning (e.g. SMW's $00:F465 sets m=1 via SEP #$20,
    # leaving caller in m=1 even though caller had m=0 pre-call). The
    # decoder previously kept caller's (m, x), causing it to mis-decode
    # subsequent operand widths and synthesise phantom branch targets
    # at mid-instruction bytes (root cause of the RunPlayerBlockCode
    # -1 stack drift / "Mario dies on slope" bug, 2026-05-03).
    #
    # p_stack is preserved across JSR/JSL: the callee's own PHP/PLP is
    # internal to its body. A well-balanced callee leaves the caller's
    # PHP/PLP stack untouched.
    if mnem in ('JSR', 'JSL'):
        ret_m, ret_x = post_m, post_x
        target_pc24: Optional[int] = None
        if mnem == 'JSR' and insn.length == 3 and insn.mode != INDIR_X:
            target_pc24 = addr24(bank, insn.operand & 0xFFFF)
        elif mnem == 'JSL':
            target_pc24 = insn.operand & 0xFFFFFF
        # JSR/JSL to an INLINE-ARGUMENT routine: the callee reads its own
        # return address off the stack, consumes the N bytes that follow
        # the call as a parameter, and advances the stacked return address
        # by N so its RTS/RTL returns past them (the classic 65816
        # inline-arg idiom; see detect_inline_arg_bytes). The recompiled
        # callee still reads those bytes correctly (the emitted JSL pushes
        # the real return PC), but the CALLER's fall-through must skip them
        # — otherwise the decoder runs straight into the argument data and
        # misdecodes it as code (Super Metroid I_RESET: the 3-byte ROM
        # pointer after `JSL APU_UploadBankIP` decoded as BRK, truncating
        # reset). Auto-detected per callee, applied at every call site — no
        # cfg hint. Skip N bytes for the return/fall-through key.
        if inline_arg_map and target_pc24 is not None:
            nskip = inline_arg_map.get(target_pc24)
            if nskip is None:
                tbank = (target_pc24 >> 16) & 0xFF
                if tbank < 0x40 or 0x80 <= tbank < 0xC0:
                    nskip = inline_arg_map.get(target_pc24 ^ 0x800000)
            if nskip:
                next_pc = (next_pc + nskip) & 0xFFFF
        callee_exit_known = False
        if callee_exit_mx is not None:
            if target_pc24 is not None:
                # Lookup keyed by (target_pc24, entry_m, entry_x) — same
                # entry variant we're invoking. Different variants of
                # the same callee may have different exit (m, x).
                key_lookup = (target_pc24, post_m, post_x)
                hit = callee_exit_mx.get(key_lookup)
                if hit is None:
                    tbank = (target_pc24 >> 16) & 0xFF
                    if tbank < 0x40 or 0x80 <= tbank < 0xC0:
                        mirror_pc24 = target_pc24 ^ 0x800000
                        hit = callee_exit_mx.get(
                            (mirror_pc24, post_m, post_x))
                if hit is not None:
                    em, ex = hit
                    if em is not None and ex is not None:
                        ret_m, ret_x = em & 1, ex & 1
                        callee_exit_known = True
        if (stop_on_unknown_callee_exit and target_pc24 is not None
                and not callee_exit_known):
            if unknown_callee_exit_sites is not None:
                item = (insn.addr & 0xFFFFFF, target_pc24,
                        post_m & 1, post_x & 1)
                if item not in unknown_callee_exit_sites:
                    unknown_callee_exit_sites.append(item)
            return []
        if (target_pc24 is not None and callee_exit_mx_modes is not None
                and (ret_m, ret_x) == (post_m, post_x)):
            mode_set = callee_exit_mx_modes.get((target_pc24, post_m, post_x))
            if mode_set is None:
                tbank = (target_pc24 >> 16) & 0xFF
                if tbank < 0x40 or 0x80 <= tbank < 0xC0:
                    mode_set = callee_exit_mx_modes.get(
                        (target_pc24 ^ 0x800000, post_m, post_x))
            if mode_set and len(mode_set) > 2:
                mode_set = None
            if mode_set and rom is not None:
                try:
                    next_off = lorom_offset(bank, next_pc)
                    next_ins = decode_insn(
                        rom, next_off, next_pc, bank, m=post_m, x=post_x)
                except Exception:
                    next_ins = None
                if next_ins is None or next_ins.mnem not in _COND_BRANCHES:
                    mode_set = None
            if mode_set:
                seen = set()
                succs = []
                for em, ex in sorted((m & 1, x & 1) for (m, x) in mode_set):
                    if (em, ex) in seen:
                        continue
                    seen.add((em, ex))
                    succs.append(
                        (DecodeKey(addr24(bank, next_pc), em, ex, post_p_stack),
                         'fall'))
                if succs:
                    return succs
        return [(DecodeKey(addr24(bank, next_pc), ret_m, ret_x, post_p_stack), 'fall')]

    # Default: linear fall-through with post-instruction mode.
    return [(DecodeKey(addr24(bank, next_pc), post_m, post_x, post_p_stack), 'fall')]


def _pea_ptrcall_return_pc(rom: Optional[bytes], bank: int, pc: int,
                           fallback_next: int) -> int:
    """Return the RTS destination for `PEA <ret_minus_1>; JMP (ptr)`.

    The dispatched handler does not return to the byte after the JMP. It
    returns through the 16-bit value pushed by PEA, so hardware resumes at
    `PEA_operand + 1`. Some state-machine dispatchers choose that to equal
    the lexical fall-through; instruction-list loops intentionally point it
    back to their loop body.
    """
    if rom is None:
        return fallback_next & 0xFFFF
    prev_pc = (pc - 3) & 0xFFFF
    try:
        off = lorom_offset(bank, prev_pc)
    except AssertionError:
        return fallback_next & 0xFFFF
    if off + 2 >= len(rom) or rom[off] != 0xF4:  # PEA abs
        return fallback_next & 0xFFFF
    pea_operand = rom[off + 1] | (rom[off + 2] << 8)
    return (pea_operand + 1) & 0xFFFF


def detect_inline_arg_bytes(rom: bytes, bank: int, addr: int,
                            entry_m: int = 1, entry_x: int = 1):
    """Detect whether the subroutine at (bank, addr) is a JSR/JSL
    INLINE-ARGUMENT routine, returning the inline byte count N (or None).

    An inline-arg routine reads its own return address off the stack, uses
    the N bytes immediately following the call site as a parameter, and
    ADVANCES the stacked return address by a constant N so its RTS/RTL
    returns PAST the inline data. The game-agnostic 65816 signature:

        LDA $rr,S        ; load the return-address-low slot (rr=1 here)
        ... (A preserved: STA dp/abs, CLC/SEC) ...
        ADC #N           ; advance the return address by N
        STA $rr,S        ; write it back to the SAME slot

    N is the count of inline bytes every call site must skip after the
    JSR/JSL (see `_labeled_successors`). Auto-detecting this from the
    callee — rather than a per-target cfg hint — is the correct fix
    (PRINCIPLES: hints are not correctness; the byte count is a property
    of the callee's own code). Canonical case: Super Metroid
    APU_UploadBankIP ($80:800A), 3-byte inline ROM pointer.

    Conservative by construction: only the exact load-add-store-back-to-
    same-slot idiom matches, and any A-clobbering instruction in between
    resets the match — so a routine that merely reads its return address
    (e.g. a JSL dispatch helper) without adding a constant and storing it
    back is NOT flagged. Returns None on the first terminator or after a
    short instruction budget."""
    from snes65816 import decode_insn, lorom_offset, STK
    # Opcodes that PRESERVE A (may appear between the load and the
    # add/store-back): stores, flag set/clear, register transfers that
    # read A, pushes, index ops, compares, NOPs.
    A_PRESERVING = {
        0x85, 0x8D, 0x8F, 0x95, 0x9D, 0x99, 0x92, 0x87, 0x97, 0x81, 0x91,
        0x9C, 0x9E,                                   # STA / STZ
        0x86, 0x8E, 0x96, 0x84, 0x8C, 0x94,           # STX / STY
        0x18, 0x38, 0xD8, 0xF8, 0x58, 0x78, 0xB8,     # CLC/SEC/CLD/SED/CLI/SEI/CLV
        0xAA, 0xA8,                                   # TAX/TAY (read A)
        0xE8, 0xC8, 0xCA, 0x88,                       # INX/INY/DEX/DEY
        0xA2, 0xA6, 0xB6, 0xAE, 0xBE,                 # LDX
        0xA0, 0xA4, 0xB4, 0xAC, 0xBC,                 # LDY
        0xE0, 0xE4, 0xEC, 0xC0, 0xC4, 0xCC,           # CPX/CPY
        0x48, 0xDA, 0x5A, 0x08, 0x8B, 0x4B, 0x0B,     # PHA/PHX/PHY/PHP/PHB/PHK/PHD
        0xEA, 0x42,                                   # NOP / WDM
    }
    pc = addr & 0xFFFF
    m, x = entry_m & 1, entry_x & 1
    a_slot = None     # stack slot the live A value came from (LDA $nn,S)
    a_added = 0       # constant added to that value since the load
    y_slot = None     # stack slot copied through Y via TAY/TYA
    y_added = 0
    budget = 0
    while budget < 96:
        budget += 1
        if not (0x8000 <= pc <= 0xFFFF):
            return None
        try:
            off = lorom_offset(bank, pc)
        except AssertionError:
            return None
        if off >= len(rom):
            return None
        try:
            ins = decode_insn(rom, off, pc, bank, m=m, x=x)
        except Exception:
            return None
        op, mn = ins.opcode, ins.mnem
        if mn in _TERMINATORS:
            return None
        if mn == 'REP':
            if ins.operand & 0x20: m = 0
            if ins.operand & 0x10: x = 0
        elif mn == 'SEP':
            if ins.operand & 0x20: m = 1
            if ins.operand & 0x10: x = 1
        elif mn == 'LDA' and ins.mode == STK:
            a_slot = ins.operand & 0xFF       # (re)start tracking from this slot
            a_added = 0
        elif op == 0xA8:                       # TAY
            if a_slot is not None:
                y_slot = a_slot
                y_added = a_added
            else:
                y_slot = None
                y_added = 0
        elif op == 0x98:                       # TYA
            if y_slot is not None:
                a_slot = y_slot
                a_added = y_added
            else:
                a_slot = None
                a_added = 0
        elif op == 0x69 and a_slot is not None:   # ADC #imm (m-dependent)
            a_added = (a_added + ins.operand) & 0xFFFF
        elif op == 0x83 and ins.mode == STK:      # STA $nn,S — return-addr write-back
            if a_slot is not None and (ins.operand & 0xFF) == a_slot and a_added:
                return a_added & 0xFF             # inline byte count
            # store-back without an add (or to a different slot): not it
        elif op in A_PRESERVING:
            if op in (0xA0, 0xA4, 0xB4, 0xAC, 0xBC, 0xC8, 0x88, 0x7A):
                y_slot = None
                y_added = 0
            pass                                  # A unchanged; keep tracking
        else:
            a_slot = None                         # A clobbered — reset
            a_added = 0
        pc = (pc + ins.length) & 0xFFFF
    return None


def classify_dispatch_helper(rom: bytes, bank: int, addr: int):
    """Identify whether the subroutine at (bank, addr) is a JSL-jump-table
    dispatch helper. Returns 'short' (16-bit table entries), 'long'
    (24-bit table entries), or None.

    Pattern (canonical SMW + general 65816 ExecutePtr-style):
      - body PULAs/PLYs the JSL return PC off the SNES stack
      - body computes a table index and JMPs through (table,X) / [abs]
      - between the first `ASL A` and the next `TAY/TAX`, the presence
        of `ADC` distinguishes 24-bit vs 16-bit entries

    Ported from v1 recomp.py:_classify_dispatch_helper. Tracks REP/SEP
    so AND #imm decodes at correct width — without that tracking, the
    AND #$FFFF in $00:86DF gets sliced into AND #$FF + BRK $0A, eating
    the ASL A that's the classifier's signature.
    """
    from snes65816 import (decode_insn, lorom_offset, ACC, INDIR, INDIR_X,
                            INDIR_L)
    insns = []
    pc = addr & 0xFFFF
    m, x = 1, 1
    safety = 0
    while safety < 256:
        safety += 1
        if not (0x8000 <= pc <= 0xFFFF):
            return None
        try:
            offset = lorom_offset(bank, pc)
        except AssertionError:
            return None
        if offset >= len(rom):
            return None
        try:
            ins = decode_insn(rom, offset, pc, bank, m=m, x=x)
        except Exception:
            return None
        if ins is None:
            return None
        insns.append(ins)
        # Update mode for subsequent decodes.
        if ins.mnem == 'REP':
            if ins.operand & 0x20: m = 0
            if ins.operand & 0x10: x = 0
        elif ins.mnem == 'SEP':
            if ins.operand & 0x20: m = 1
            if ins.operand & 0x10: x = 1
        if ins.mnem in ('RTS', 'RTL', 'RTI', 'BRA', 'BRL', 'JMP', 'JML', 'STP'):
            break
        pc = (pc + ins.length) & 0xFFFF

    if not insns:
        return None
    # Must pull return address off stack.
    if not any(i.mnem in ('PLA', 'PLY') for i in insns):
        return None
    # Must end with an indirect jump.
    last = insns[-1]
    if not (last.mnem in ('JMP', 'JML') and
            last.mode in (INDIR, INDIR_X, INDIR_L)):
        return None
    # Width: ASL A ... TAY/TAX, with ADC in between → long.
    asl_seen = False
    has_adc = False
    for ins in insns:
        if not asl_seen:
            if ins.mnem == 'ASL' and ins.mode == ACC:
                asl_seen = True
            continue
        if ins.mnem == 'ADC':
            has_adc = True
        if ins.mnem in ('TAY', 'TAX'):
            return 'long' if has_adc else 'short'
    return None


# ── decode_function memoization cache ───────────────────────────────
#
# decode_function is pure for fixed inputs: no module-level mutable
# state is read, and grep verifies external callers never mutate the
# returned FunctionDecodeGraph (only decoder.py itself writes to
# graph.insns inside the worklist). Returned graphs are therefore safe
# to share across callers via cache. Callers must not mutate; doing so
# poisons the cached entry for every subsequent caller with the same
# key.
#
# Key composition: immutable ROM bytes + primitive args + a strong-reference
# identity token for every Optional dict/list/set.
#
# Why hash(rom): rom bytes are immutable; CPython caches the hash on
# the bytes object after first compute (O(n) once, O(1) thereafter).
# id(rom) would NOT be safe across multiple bytes objects in one
# process — Python may reuse the address of a GC'd object.
#
# Why identity for dict/list/set kwargs (NOT _freeze): freezing big dicts
# (callee_exit_mx in a late auto-promote pass has thousands of items)
# is O(N) per decode call. With ~30k decode calls per pass × full
# freeze of a 10k-item dict per call, the freeze dominates wall-clock
# and makes A1 a regression vs the pre-cache pipeline. id() is O(1).
# A bare id() still permits address reuse because the integer does not retain
# the object. _ObjectIdentity below owns a strong reference and compares by
# identity, closing that collision without an O(N) semantic freeze.
# Callers must not mutate a kwarg dict while passing it to
# decode_function — that would poison the cache silently.
#
# **Memory bounding is the caller's responsibility.** Different
# pipeline phases pass different kwarg combinations, so the same
# (entry, m, x) tuple gets cached under N keys across N phases.
# v2_regen.py calls clear_decode_cache() at every phase boundary.

_DECODE_CACHE: Dict[tuple, "FunctionDecodeGraph"] = {}
_DECODE_CACHE_HITS = 0
_DECODE_CACHE_MISSES = 0
_DECODE_CACHE_ENABLED = False  # v2_regen enables unless --no-decode-cache

# Process-wide default inline-argument map (see detect_inline_arg_bytes /
# _labeled_successors). v2_regen builds it once per regen and installs it
# here so EVERY decode_function caller in the process picks it up without
# threading the param through every pre-pass call site. An explicit
# `inline_arg_map=` arg still overrides this (used on the multiprocessing
# emit-worker path, which can't see a main-process global). None => no
# inline-arg routines known (pre-detection / no callers declared).
_ACTIVE_INLINE_ARG_MAP: Optional[Dict[int, int]] = None


def set_active_inline_arg_map(m: Optional[Dict[int, int]]) -> None:
    """Install the process-wide default inline-arg map used as the
    fall-back when decode_function is called without an explicit
    `inline_arg_map`. Pass None/{} to clear."""
    global _ACTIVE_INLINE_ARG_MAP
    _ACTIVE_INLINE_ARG_MAP = m or None


def set_decode_cache_enabled(enabled: bool) -> None:
    """Enable/disable decode_function memoization. Disabling clears."""
    global _DECODE_CACHE_ENABLED
    _DECODE_CACHE_ENABLED = bool(enabled)
    if not _DECODE_CACHE_ENABLED:
        clear_decode_cache()


def clear_decode_cache() -> None:
    """Drop every cached decode result and reset counters."""
    global _DECODE_CACHE_HITS, _DECODE_CACHE_MISSES
    _DECODE_CACHE.clear()
    _DECODE_CACHE_HITS = 0
    _DECODE_CACHE_MISSES = 0


def decode_cache_stats() -> Dict[str, int]:
    """Snapshot of {enabled, hits, misses, size}."""
    return {
        "enabled": 1 if _DECODE_CACHE_ENABLED else 0,
        "hits": _DECODE_CACHE_HITS,
        "misses": _DECODE_CACHE_MISSES,
        "size": len(_DECODE_CACHE),
    }


def _freeze(obj):
    """Recursively convert dict/list/set into a hashable, deterministic
    representation for cache keys. dicts are sorted by repr(key) so two
    dicts with the same content but different insertion order produce
    the same frozen form. Handles tuple keys (callee_exit_mx uses
    (pc24, m, x) keys) via repr-sort."""
    if obj is None:
        return None
    if isinstance(obj, (int, str, bool, float, bytes)):
        return obj
    if isinstance(obj, tuple):
        return tuple(_freeze(x) for x in obj)
    if isinstance(obj, frozenset):
        return frozenset(_freeze(x) for x in obj)
    if isinstance(obj, dict):
        return tuple(sorted(
            ((_freeze(k), _freeze(v)) for k, v in obj.items()),
            key=lambda kv: repr(kv[0]),
        ))
    if isinstance(obj, list):
        return tuple(_freeze(x) for x in obj)
    if isinstance(obj, set):
        return frozenset(_freeze(x) for x in obj)
    return obj


class _ObjectIdentity:
    """Hashable, strong-reference identity token for context objects.

    A bare ``id(obj)`` cache key does not retain ``obj``, allowing Python to
    reuse that address after a pipeline phase replaces a mapping.  This token
    makes address reuse impossible without paying an O(N) content-freeze on
    every decode.  Context mappings are immutable snapshots within a cache
    phase; callers replace the snapshot or clear the cache when facts change.
    """

    __slots__ = ('obj', '_hash')

    def __init__(self, obj):
        self.obj = obj
        self._hash = id(obj)

    def __hash__(self):
        return self._hash

    def __eq__(self, other):
        return isinstance(other, _ObjectIdentity) and self.obj is other.obj


def _identity(obj):
    return _ObjectIdentity(obj) if obj is not None else None


def decode_function(rom: bytes, bank: int, start: int,
                    entry_m: int, entry_x: int,
                    *, end: Optional[int] = None,
                    max_insns: int = 12000,
                    dispatch_helpers: Optional[Dict[int, str]] = None,
                    indirect_call_tables: Optional[Dict[int, dict]] = None,
                    indirect_dispatch: Optional[Dict[int, dict]] = None,
                    hle_dispatch: Optional[Dict[int, str]] = None,
                    data_regions: Optional[List[Tuple[int, int, int]]] = None,
                    callee_exit_mx: Optional[Dict] = None,
                    callee_exit_mx_modes: Optional[Dict] = None,
                    sibling_entry_pcs: Optional[set] = None,
                    inline_arg_map: Optional[Dict[int, int]] = None,
                    stop_on_unknown_callee_exit: bool = False,
                    ) -> "FunctionDecodeGraph":
    """Public cached wrapper around `_decode_function_uncached`.

    See `_decode_function_uncached` for decoder semantics. The cache is
    process-local; callers must treat the returned graph as immutable
    (it is shared across cache hits with the same key). v2_regen.py
    calls clear_decode_cache() between phases to bound memory."""
    if inline_arg_map is None:
        inline_arg_map = _ACTIVE_INLINE_ARG_MAP
    if not _DECODE_CACHE_ENABLED:
        return _decode_function_uncached(
            rom, bank, start, entry_m, entry_x,
            end=end, max_insns=max_insns,
            dispatch_helpers=dispatch_helpers,
            indirect_call_tables=indirect_call_tables,
            indirect_dispatch=indirect_dispatch,
            hle_dispatch=hle_dispatch,
            data_regions=data_regions,
            callee_exit_mx=callee_exit_mx,
            callee_exit_mx_modes=callee_exit_mx_modes,
            sibling_entry_pcs=sibling_entry_pcs,
            inline_arg_map=inline_arg_map,
            stop_on_unknown_callee_exit=stop_on_unknown_callee_exit,
        )

    cache_key = (
        rom, bank, start, entry_m & 1, entry_x & 1, end, max_insns,
        _identity(dispatch_helpers),
        _identity(indirect_call_tables),
        _identity(indirect_dispatch),
        _identity(hle_dispatch),
        _identity(data_regions),
        _identity(callee_exit_mx),
        _identity(callee_exit_mx_modes),
        _identity(sibling_entry_pcs),
        _identity(inline_arg_map),
        bool(stop_on_unknown_callee_exit),
    )
    cached = _DECODE_CACHE.get(cache_key)
    if cached is not None:
        global _DECODE_CACHE_HITS
        _DECODE_CACHE_HITS += 1
        return cached

    global _DECODE_CACHE_MISSES
    _DECODE_CACHE_MISSES += 1
    graph = _decode_function_uncached(
        rom, bank, start, entry_m, entry_x,
        end=end, max_insns=max_insns,
        dispatch_helpers=dispatch_helpers,
        indirect_call_tables=indirect_call_tables,
        indirect_dispatch=indirect_dispatch,
        hle_dispatch=hle_dispatch,
        data_regions=data_regions,
        callee_exit_mx=callee_exit_mx,
        callee_exit_mx_modes=callee_exit_mx_modes,
        sibling_entry_pcs=sibling_entry_pcs,
        inline_arg_map=inline_arg_map,
        stop_on_unknown_callee_exit=stop_on_unknown_callee_exit,
    )
    _DECODE_CACHE[cache_key] = graph
    return graph


def _decode_function_uncached(rom: bytes, bank: int, start: int,
                    entry_m: int, entry_x: int,
                    *, end: Optional[int] = None,
                    max_insns: int = 12000,
                    dispatch_helpers: Optional[Dict[int, str]] = None,
                    indirect_call_tables: Optional[Dict[int, dict]] = None,
                    indirect_dispatch: Optional[Dict[int, dict]] = None,
                    hle_dispatch: Optional[Dict[int, str]] = None,
                    data_regions: Optional[List[Tuple[int, int, int]]] = None,
                    callee_exit_mx: Optional[Dict] = None,
                    callee_exit_mx_modes: Optional[Dict] = None,
                    sibling_entry_pcs: Optional[set] = None,
                    inline_arg_map: Optional[Dict[int, int]] = None,
                    stop_on_unknown_callee_exit: bool = False,
                    ) -> FunctionDecodeGraph:
    """Decode a function starting at (bank, start) with entry (m, x) state.

    Worklist over DecodeKey tuples. Each key is decoded at most once;
    same PC with divergent (m, x) produces multiple keys → multiple
    DecodedInsn records.

    `dispatch_helpers`: optional map of {target_addr_24 -> 'short'|'long'}.
    When a JSL/JML hits a target in this map, the bytes immediately AFTER
    the JSL are decoded as a function-pointer TABLE (not as instructions).
    Each table entry is recorded as a successor key (so the dispatched
    handlers get decoded too) and the ORIGINAL JSL is marked with
    `insn.dispatch_entries` for downstream codegen. Decode resumes
    AFTER the table (not at the JSL+length offset). Without this hook,
    SMW's "JSL Foo; .dw target0, target1, ..." pattern at $00:9325 would
    decode the TABLE BYTES as garbage instructions.

    `indirect_call_tables`: optional map of {site_pc24 -> dict} where
    each value is `{'base': int, 'count': int, 'kind': 'short'|'long'}`.
    Authorises a JSR (abs,X) site as a real indirect dispatch. When
    set, the decoder reads `count` table entries at `bank:base`,
    stamps `insn.dispatch_entries`, and adds the entries as decode
    successors so handlers get decoded too. Without an entry, JSR
    (abs,X) is treated as cfg-unauthorised: the insn is placed in the
    graph with no successors (severing fall-through) and recorded in
    graph.suppressed_indirect_calls for the build report. See the
    cfg-required-dispatch-or-kill rule documented on
    SuppressedIndirectCall.

    `sibling_entry_pcs`: optional set of 16-bit PCs in THIS bank that
    are named function entries OTHER than `start`. When a `jump` edge
    crosses end: and lands on a sibling entry, the decoder refuses the
    inline-import — the boundary becomes a tail-call handled by
    emit_function's `_goto_or_return`. Without this gate, the inline-
    cross-fn-blocks model would import the sibling's entire body into
    THIS function's CFG (the Zelda intro-loop root cause 2026-05-17:
    Intro_Init_Continue's BCS to Intro_InitializeMemory_darken at
    $0C:C1F5 inlined darken into Intro_Init_Continue's body, so darken's
    `submodule_index++` ran on a wrong path and submodule oscillated
    0→1→2→0 instead of progressing).

    The PHB/PLB-balanced cross-fn-jump case is preserved because those
    targets are NOT named function entries — the inline-import path
    still applies. Only cfg-named entries get the tail-call routing.
    """
    entry_m &= 1
    entry_x &= 1
    entry_key = DecodeKey(addr24(bank, start), entry_m, entry_x)
    graph = FunctionDecodeGraph(entry=entry_key)

    # Worklist holds (key, edge_kind, pred_pc). edge_kind is
    # 'entry' for the initial seed, 'jump' for BRA/BRL/JMP-ABS/cond-
    # branch-target, 'fall' for linear next-PC after non-control or
    # JSR/JSL. pred_pc is the predecessor PC (-1 for entry seed).
    #
    # end: gates the boundary CROSSING, not the imported territory:
    # we reject a fall-through edge whose SOURCE is inside [start,end)
    # and whose TARGET is past end:. That stops natural drift of the
    # entry's body into the NEXT cfg function. Inside imported
    # territory (source.pc >= end, reached via prior 'jump'), all
    # successors are decoded — fall-through within an imported routine
    # is part of that routine's lifetime, not a boundary crossing.
    worklist: List = [(entry_key, 'entry', -1)]

    while worklist:
        if len(graph.insns) >= max_insns:
            raise RuntimeError(
                f"v2 decoder exceeded max_insns={max_insns} at "
                f"function ${addr24(bank, start):06X}"
            )
        key, edge_kind, pred_pc = worklist.pop()
        if key in graph.insns:
            continue

        pc = key.pc & 0xFFFF
        # Boundary-crossing fall-through: predecessor was inside the
        # nominal range, and this fall-through would land past end: in
        # the next function's body. Reject — that's exactly what end:
        # was put in cfg to prevent. (Jump targets past end: were
        # already accepted by the same predecessor; this only blocks
        # the unintended drift.)
        if (end is not None
                and pc >= end
                and edge_kind == 'fall'
                and pred_pc >= 0
                and pred_pc < end):
            continue
        # JUMP edge that lands on a named sibling function entry:
        # refuse the inline-import. emit_function's `_goto_or_return`
        # then emits a tail-call to the sibling (recompiler-level fix
        # 2026-05-17, generalised 2026-05-22). Without this, the
        # inline-cross-fn-blocks model would pull the sibling's entire
        # body into THIS function's CFG — root cause of the Zelda intro
        # submodule oscillation (Intro_Init_Continue's BCS to Intro_
        # InitializeMemory_darken inlined darken's submodule_index++),
        # and root cause of MMX TaskDie ($00:80F8) being inlined into
        # every task body's tail (the asm scheduler walk overwrote
        # slot state on every yield).
        #
        # The earlier form gated this on `pc >= end` + `pred_pc < end`,
        # which only matched siblings ABOVE the current function in PC
        # space. MMX's TaskDie sits BELOW its callers ($80F8 < $852C,
        # $B091, $B25B, ...) and slipped through. The address ordering
        # is incidental — if the cfg declares pc as another function's
        # entry, every cross-fn jump to that pc is by definition a
        # tail-call. sibling_entry_pcs already excludes THIS function's
        # own start, so back-edges to entry are unaffected. The PHB/
        # PLB-balanced cross-fn-jump case is unaffected — those targets
        # aren't in `sibling_entry_pcs`.
        if (sibling_entry_pcs is not None
                and edge_kind == 'jump'
                and pc in sibling_entry_pcs):
            continue
        if not (0x8000 <= pc <= 0xFFFF):
            # Out-of-bank reference; surface upstream by skipping here.
            continue

        try:
            offset = lorom_offset(bank, pc)
        except AssertionError:
            continue
        if offset >= len(rom):
            continue

        insn = decode_insn(rom, offset, pc, bank, m=key.m, x=key.x)
        if insn is None:
            raise ValueError(
                f"v2 decoder: unknown opcode ${rom[offset]:02X} at "
                f"${bank:02X}:{pc:04X} entry_mx=({key.m},{key.x})"
            )

        # Stamp entry mode on the Insn so downstream consumers (cfg, IR,
        # codegen) see the entry state without needing the DecodeKey.
        insn.m_flag = key.m
        insn.x_flag = key.x

        # JSL/JML dispatch-table detection: if the call target is a
        # registered dispatch helper, decode the bytes immediately
        # following as the target table and record successors.
        is_jsl_or_jml = (insn.mnem == 'JSL' or
                         (insn.mnem == 'JMP' and insn.length == 4))  # JML
        helper_kind = None
        if dispatch_helpers and is_jsl_or_jml:
            helper_kind = dispatch_helpers.get(insn.operand & 0xFFFFFF)
        if helper_kind is not None:
            entries = []
            entry_size = 3 if helper_kind == 'long' else 2
            tbl_pc = (pc + insn.length) & 0xFFFF
            while len(entries) < 256 and tbl_pc + entry_size - 1 <= 0xFFFF:
                try:
                    tbl_off = lorom_offset(bank, tbl_pc)
                except AssertionError:
                    break
                if tbl_off + entry_size - 1 >= len(rom):
                    break
                lo = rom[tbl_off]
                hi = rom[tbl_off + 1]
                addr16 = lo | (hi << 8)
                if helper_kind == 'long':
                    eb = rom[tbl_off + 2]
                    if addr16 == 0 and eb == 0:
                        entries.append(0)
                        tbl_pc += entry_size
                        continue
                    # Long-format dispatch tables can target ANY bank —
                    # the whole point of 24-bit entries is cross-bank
                    # dispatch. The earlier `eb != bank` reject was too
                    # strict; it truncated zelda3's Intro_Init_Continue
                    # dispatch table at 8 entries instead of 11
                    # (Intro_LoadTextPointersAndPalettes at $02:8116,
                    # LoadItemGFXIntoWRAM4BPPBuffer at $00:D231,
                    # LoadFollowerGraphics at $00:D423 all rejected).
                    # The intro pipeline would skip these init handlers
                    # — root cause of the Nintendo-jingle endless-loop
                    # (subsubmodule 8/9/10 dispatched to nothing).
                    # Replacement check: accept any valid LoROM bank
                    # ($00-$3F or $80-$FF) with addr16 >= 0x8000. The
                    # `_dispatch_target_is_padding` gate below catches
                    # genuine table-end junk.
                    if addr16 < 0x8000:
                        break
                    is_valid_lorom_bank = (eb < 0x40) or (eb >= 0x80)
                    if not is_valid_lorom_bank:
                        break
                    # Validity gate: stop the table if the entry points
                    # into all-FF or all-00 bytes. See
                    # `_dispatch_target_is_padding` doc.
                    if _dispatch_target_is_padding(rom, eb, addr16):
                        break
                    # cfg `data_region:` gate — explicit ROM fact that
                    # (bank, pc16) range is data. Trumps any addr-range
                    # heuristic since the directive is ground truth.
                    if _addr_in_data_regions(data_regions, eb, addr16):
                        graph.dispatch_targets_suppressed.append(
                            DispatchTargetSuppressed(
                                site_pc24=(bank << 16) | pc,
                                target_pc24=(eb << 16) | addr16,
                                reason='data_region',
                                table_index=len(entries),
                            ))
                        break
                    full_entry = (eb << 16) | addr16
                else:
                    if addr16 == 0:
                        entries.append(0)
                        tbl_pc += entry_size
                        continue
                    if addr16 < 0x8000:
                        break
                    if _dispatch_target_is_padding(rom, bank, addr16):
                        break
                    if _addr_in_data_regions(data_regions, bank, addr16):
                        graph.dispatch_targets_suppressed.append(
                            DispatchTargetSuppressed(
                                site_pc24=(bank << 16) | pc,
                                target_pc24=(bank << 16) | addr16,
                                reason='data_region',
                                table_index=len(entries),
                            ))
                        break
                    full_entry = (bank << 16) | addr16
                # NOTE: do NOT bound the entry value by the dispatching
                # function's [start, end) range. The TABLE bytes live in
                # the function's range, but the table ENTRIES point to
                # OTHER handlers (e.g. GameMode00 at \$00:9391, well past
                # the dispatcher's $937D end). v1's recomp.py applied a
                # similar range check ONLY as a fallback when the entry
                # wasn't in `dispatch_known_addrs`; v2 doesn't have that
                # set yet, so any range bound here would terminate the
                # table at zero entries — exactly what was happening to
                # the SMW GameMode dispatch at $00:9325 before this fix.
                entries.append(full_entry if helper_kind == 'long' else addr16)
                tbl_pc += entry_size
            if entries:
                # Stash on the insn for codegen. Don't add dispatch
                # entries as decode successors — they're CROSS-FUNCTION
                # calls (auto-promote will pick them up). The JSL itself
                # is a TERMINATOR (no fall-through past the table because
                # the dispatcher returns to the dispatched handler's
                # caller, not to bytes after the JSL).
                insn.dispatch_entries = entries
                insn.dispatch_kind = helper_kind
                graph.insns[key] = DecodedInsn(key=key, insn=insn, successors=[])
                continue

        # cfg `indirect_dispatch` for JMP / JML indirect — recovers the
        # static target list of an IndirectGoto. Class fix for the
        # "IndirectGoto: dispatch table" stub class (Zelda Module_MainRouting
        # boot blocker + ~50 other sites; SMW 2 sites).
        #
        # Form-handling:
        #   - JMP (abs,X) / JML (abs,X)  — single table at insn.operand,
        #     entry size from JMP/JML width (16 or 24 bit). AUTO-RECOVERED
        #     when no cfg directive exists: walk entries at insn.operand
        #     until one falls outside the bank, points into data_region,
        #     or looks like padding. cfg can override.
        #   - JMP (abs) [opcode 6C]      — single fixed-target indirect
        #     (16-bit). Static recovery needs cfg (no table to walk).
        #   - JML [abs]  [opcode DC]     — single fixed-target indirect
        #     (24-bit). Same caveat as JMP (abs).
        #   - JMP/JML [DP] — DP-built pointer (Module_MainRouting form).
        #     cfg supplies tables: <lo>[,<hi>[,<bank>]] — 1, 2 or 3
        #     parallel byte-tables forming a 16/24-bit pointer per
        #     dispatch index.
        if (insn.mnem in ('JMP', 'JML')
                and insn.mode in (INDIR, INDIR_X)):
            site_pc24 = (bank << 16) | pc
            auth = (indirect_dispatch or {}).get(site_pc24)
            # Auto-recovery for (abs,X) form: walk the table at the
            # operand until invalid. Skipped when cfg already authorised
            # the site (cfg overrides — same count, but explicit beats
            # heuristic).
            if auth is None and insn.mode == INDIR_X:
                entries = _autorecover_indirect_xtable(rom, bank, insn,
                                                       data_regions,
                                                       func_start=start)
                if entries:
                    auth = {
                        'count': len(entries),
                        'idx_reg': 'X',
                        'table_bases': (),
                        '_autorecovered': True,
                    }
            # Auto-recovery for (abs) / [abs] DP-built-pointer form:
            # walk back from func start to find LDA <tbl>,<idx> /
            # STA $<dp+k> pairs that compose the dispatch pointer
            # immediately before the JMP/JML. count comes from a
            # walk-until-invalid pass over the recovered tables.
            if auth is None and insn.mode == INDIR:
                # Operand is the abs addr the JMP/JML indirects through.
                # For DP-resident pointer (most common in zelda3), op
                # is < $0100 and we can walk-back to find tables.
                dp_op = insn.operand & 0xFFFF
                if 0x0000 <= dp_op <= 0x00FF:
                    rec = _autorecover_indirect_dp(
                        rom, bank, start, pc, dp_op,
                        insn.length, data_regions=data_regions)
                    if rec is not None:
                        table_bases, idx_reg = rec
                        # Count from walk-until-invalid over the
                        # recovered tables.
                        count = _autorecover_dp_table_count(
                            rom, bank, table_bases, data_regions)
                        if count:
                            auth = {
                                'count': count,
                                'idx_reg': idx_reg,
                                'table_bases': table_bases,
                                '_autorecovered': True,
                            }
                if auth is None:
                    entries = _autorecover_local_stride_runway(
                        rom, bank, start, pc, insn,
                        end=end, data_regions=data_regions)
                    if entries:
                        auth = {
                            'count': len(entries),
                            'idx_reg': 'X',
                            'table_bases': (insn.operand & 0xFFFF,),
                            'targets': tuple(e & 0xFFFF for e in entries),
                            'local_goto': True,
                            '_autorecovered': True,
                        }
            # Static single-target form: `JMP ($<abs>)` / `JML [$<abs>]`
            # where <abs> is in ROM range ($8000+). The pointer lives
            # directly in ROM at (bank, abs) — read it once at decode
            # time and treat the site as a 1-entry dispatch. Equivalent
            # to a plain JMP to the read target, but produced as a
            # dispatch so the same emit path applies.
            if (auth is None and insn.mode == INDIR
                    and (insn.operand & 0xFFFF) >= 0x8000):
                tbl_pc = insn.operand & 0xFFFF
                entry_size = 3 if _dispatch_kind(insn) == 'long' else 2
                try:
                    tbl_off = lorom_offset(bank, tbl_pc)
                    rom_ok = tbl_off + entry_size - 1 < len(rom)
                except AssertionError:
                    rom_ok = False
                if rom_ok:
                    tgt_lo = rom[tbl_off]
                    tgt_hi = rom[tbl_off + 1]
                    tgt16 = tgt_lo | (tgt_hi << 8)
                    tgt_bank = rom[tbl_off + 2] if entry_size == 3 else bank
                    if (tgt16 >= 0x8000
                            and not _addr_in_data_regions(
                                data_regions, tgt_bank, tgt16)
                            and not _dispatch_target_is_padding(
                                rom, tgt_bank, tgt16)):
                        auth = {
                            'count': 1,
                            # Index isn't really used (count=1), but the
                            # emit path requires X or Y. X is harmless;
                            # the runtime branch is `if (idx >= 1) trap;
                            # switch (0)`, which collapses to the call.
                            'idx_reg': 'X',
                            'table_bases': (tbl_pc,),
                            '_autorecovered': True,
                            '_single_target': True,
                        }
            if auth is not None:
                entries = _resolve_indirect_dispatch_targets(
                    rom, bank, insn, auth)
                if entries is not None:
                    is_ptr_call = bool(auth.get('ptr_call'))
                    is_local_goto = bool(auth.get('local_goto'))
                    insn.dispatch_entries = entries
                    insn.dispatch_kind = _dispatch_kind(
                        insn, auth.get('table_bases', ()))
                    insn.dispatch_idx_reg = auth['idx_reg']
                    insn.dispatch_local_goto = is_local_goto
                    if is_ptr_call:
                        # Value-matched switch on the pointer loaded from the
                        # operand address (codegen INDIR path keys on a single
                        # table_base to read `cpu_read16(PB, operand)`).
                        insn.dispatch_table_bases = (insn.operand & 0xFFFF,)
                        insn.dispatch_call = True
                    elif is_local_goto:
                        # Value-matched switch on the computed pointer, but
                        # targets are labels inside this generated function.
                        insn.dispatch_table_bases = (insn.operand & 0xFFFF,)
                    else:
                        insn.dispatch_table_bases = tuple(auth.get('table_bases', ()) or ())
                    # Register each in-bank target as a decode successor
                    # so reach-analysis + auto-promote pick up the handlers.
                    extra_succs = []
                    site_m = insn.m_flag & 1
                    site_x = insn.x_flag & 1
                    for e in entries:
                        if e is None or e == 0:
                            continue
                        eb = (e >> 16) & 0xFF
                        e16 = e & 0xFFFF
                        if eb == bank and 0x8000 <= e16 <= 0xFFFF:
                            extra_succs.append(
                                (DecodeKey(addr24(eb, e16), site_m, site_x, ()),
                                 'jump'))
                    # JMP/JML indirect is normally a TERMINATOR (no
                    # fall-through). The pointer-sourced CALL form (PEA <ret>;
                    # JMP (ptr)) is non-terminal: the dispatched handler RTSes
                    # to the PEA'd return, resuming the next sequential block —
                    # so add it as a fall-through successor.
                    succ = [k for (k, _) in extra_succs]
                    decode_succs = list(extra_succs)
                    if is_ptr_call:
                        nxt = _pea_ptrcall_return_pc(
                            rom, bank, pc, (pc + insn.length) & 0xFFFF)
                        nxt_key = DecodeKey(addr24(bank, nxt), site_m, site_x, ())
                        succ = [nxt_key]
                        decode_succs.append((nxt_key, 'fall'))
                    graph.insns[key] = DecodedInsn(key=key, insn=insn,
                                                   successors=succ)
                    for s, sk in decode_succs:
                        if s not in graph.insns:
                            worklist.append((s, sk, pc))
                    continue

        # Indirect JMP / JML reached here ⇒ no cfg authorisation. Record
        # as unresolved (v2_regen hard-fails on any). The insn stays in
        # the graph with no successors so predecessors' edges still
        # resolve — same shape as suppressed JSR (abs,X).
        #
        # Exception: cfg `hle_dispatch <pc16> <c_helper>` claims the site
        # for a host-side dispatcher. Treat as terminal (same shape, no
        # successors) but DO NOT record as unresolved — emit_function
        # will emit a tail-call to the helper from every caller-body
        # that inlined this dispatch.
        if (insn.mnem in ('JMP', 'JML')
                and insn.mode in (INDIR, INDIR_X)):
            if hle_dispatch and (pc & 0xFFFF) in hle_dispatch:
                helper = hle_dispatch[pc & 0xFFFF]
                # Reserved balanced-interpreter sites may use either hardware
                # dynamic-call idiom:
                #   PEA <ret-1>; JMP (ptr)       (2-byte RTS frame)
                #   PHK; PEA <ret-1>; JML [ptr] (3-byte RTL frame)
                # The dynamic callee consumes the already-pushed frame and
                # compiled execution resumes at the PEA destination.
                pushed_call_size = 0
                if helper == '__balanced_interp__' and rom is not None:
                    try:
                        pea = lorom_offset(bank, (pc - 3) & 0xFFFF)
                        if (pea + 2 < len(rom) and rom[pea] == 0xF4):
                            pushed_call_size = 2
                            try:
                                phk = lorom_offset(bank, (pc - 4) & 0xFFFF)
                                if phk < len(rom) and rom[phk] == 0x4B:
                                    pushed_call_size = 3
                            except AssertionError:
                                pass
                    except AssertionError:
                        pushed_call_size = 0
                if pushed_call_size:
                    site_m = insn.m_flag & 1
                    site_x = insn.x_flag & 1
                    nxt = _pea_ptrcall_return_pc(
                        rom, bank, pc, (pc + insn.length) & 0xFFFF)
                    nxt_key = DecodeKey(addr24(bank, nxt), site_m, site_x, ())
                    insn.dispatch_pushed_call = True
                    insn.dispatch_pushed_call_frame_size = pushed_call_size
                    insn.dispatch_return_pc = nxt
                    insn.dispatch_return_m = site_m
                    insn.dispatch_return_x = site_x
                    graph.insns[key] = DecodedInsn(
                        key=key, insn=insn, successors=[nxt_key])
                    if nxt_key not in graph.insns:
                        worklist.append((nxt_key, 'fall', pc))
                else:
                    graph.insns[key] = DecodedInsn(
                        key=key, insn=insn, successors=[])
                continue
            graph.insns[key] = DecodedInsn(key=key, insn=insn, successors=[])
            graph.unresolved_indirects.append(UnresolvedIndirect(
                site_pc24=(bank << 16) | pc,
                mnem=insn.mnem,
                mode=insn.mode,
                operand=insn.operand & 0xFFFFFF,
                function_entry_pc24=addr24(bank, start),
                entry_m=key.m,
                entry_x=key.x,
            ))
            continue

        # cfg `indirect_dispatch` for RTS-stack dispatchers. Some 65816
        # code computes an index, loads a 16-bit handler address from a
        # ROM table, decrements it, PHA's it, then SEP #$30 + RTS. The
        # RTS adds one to the pulled address, so this is a tail-call
        # through the table rather than an ordinary stack push. When cfg
        # authorises the PHA site, stamp the PHA as a terminal dispatch
        # so emit_function replaces the literal push with a switch and
        # does not leak the synthetic return address onto the simulated
        # SNES stack.
        if insn.mnem == 'PHA':
            site_pc24 = (bank << 16) | pc
            auth = (indirect_dispatch or {}).get(site_pc24)
            if auth is not None:
                entries = _resolve_indirect_dispatch_targets(
                    rom, bank, insn, auth)
                if entries is not None:
                    insn.dispatch_entries = entries
                    insn.dispatch_kind = _dispatch_kind(
                        insn, auth.get('table_bases', ()))
                    insn.dispatch_idx_reg = auth['idx_reg']
                    insn.dispatch_table_bases = tuple(auth.get('table_bases', ()) or ())
                    insn.dispatch_terminal = True
                    labeled_succ = []
                    for e in entries:
                        if e is None or e == 0:
                            continue
                        eb = (e >> 16) & 0xFF
                        e16 = e & 0xFFFF
                        if eb == bank and 0x8000 <= e16 <= 0xFFFF:
                            labeled_succ.append(
                                (DecodeKey(addr24(eb, e16), 1, 1, ()),
                                 'jump'))
                    succ = [k for (k, _) in labeled_succ]
                    graph.insns[key] = DecodedInsn(key=key, insn=insn,
                                                   successors=succ)
                    for s, sk in labeled_succ:
                        if s not in graph.insns:
                            worklist.append((s, sk, pc))
                    continue

        # cfg-required-dispatch-or-kill for JSR (abs,X). See class
        # SuppressedIndirectCall above and the regression test at
        # tests/v2/test_decoder_smc_phantom_suppression.py.
        if insn.mnem == 'JSR' and insn.mode == INDIR_X:
            site_pc24 = (bank << 16) | pc
            # Prefer the unified `indirect_dispatch` directive (new path,
            # 2026-05-17 class fix). Fall back to the legacy
            # `indirect_call_table` map for compat. If the unified
            # directive matches, recover targets through the same helper
            # the JMP path uses — keeps semantics symmetric.
            ud_auth = (indirect_dispatch or {}).get(site_pc24)
            # 2026-05-18 class fix: auto-recover same-bank JSR (abs,X)
            # dispatch tables when cfg has no authorisation. Mirrors the
            # JMP (abs,X) auto-recovery flow above — walks the table at
            # the operand until the first invalid entry, applies the
            # same structural gating (in-bank PC range, data_region
            # check, padding check, null-pair stop). Closes the
            # `Call indirect SUPPRESSED` stub class without per-site
            # cfg `indirect_dispatch` declarations. cfg still wins when
            # present (explicit beats heuristic).
            if ud_auth is None:
                entries = _autorecover_indirect_xtable(rom, bank, insn,
                                                       data_regions,
                                                       func_start=start)
                if entries:
                    ud_auth = {
                        'count': len(entries),
                        'idx_reg': 'X',
                        'table_bases': (),
                        '_autorecovered': True,
                    }
            if ud_auth is not None:
                entries = _resolve_indirect_dispatch_targets(
                    rom, bank, insn, ud_auth)
                if entries is not None:
                    is_ptr_call = bool(ud_auth.get('ptr_call'))
                    kind = _dispatch_kind(
                        insn, ud_auth.get('table_bases', ()))
                    insn.dispatch_entries = entries
                    insn.dispatch_kind = kind
                    insn.dispatch_idx_reg = ud_auth['idx_reg']
                    if is_ptr_call:
                        # Explicit target-list JSR (abs,X): X points at a
                        # runtime descriptor/pointer, not a contiguous ROM
                        # dispatch table. Codegen must switch on the loaded
                        # pointer value rather than X / entry_size.
                        insn.dispatch_table_bases = (insn.operand & 0xFFFF,)
                        insn.dispatch_call = True
                    else:
                        insn.dispatch_table_bases = tuple(ud_auth.get('table_bases', ()) or ())
                    labeled_succ = _labeled_successors(
                        insn, key, bank,
                        callee_exit_mx=callee_exit_mx,
                        callee_exit_mx_modes=callee_exit_mx_modes,
                        rom=rom, inline_arg_map=inline_arg_map,
                        stop_on_unknown_callee_exit=(
                            stop_on_unknown_callee_exit),
                        unknown_callee_exit_sites=(
                            graph.unknown_callee_exit_sites))
                    site_m = insn.m_flag & 1
                    site_x = insn.x_flag & 1
                    for e in entries:
                        if e is None or e == 0:
                            continue
                        eb = (e >> 16) & 0xFF
                        e16 = e & 0xFFFF
                        if eb == bank and 0x8000 <= e16 <= 0xFFFF:
                            labeled_succ.append(
                                (DecodeKey(addr24(eb, e16), site_m, site_x, ()),
                                 'jump'))
                    succ = [k for (k, _) in labeled_succ]
                    graph.insns[key] = DecodedInsn(key=key, insn=insn,
                                                   successors=succ)
                    for s, sk in labeled_succ:
                        if s not in graph.insns:
                            worklist.append((s, sk, pc))
                    continue
            auth = (indirect_call_tables or {}).get(site_pc24)
            if auth is not None:
                # AUTHORISED: read the static dispatch table from
                # `bank:base`, register entries as decode successors,
                # stamp `insn.dispatch_entries` for codegen.
                base = int(auth['base']) & 0xFFFF
                count = int(auth['count'])
                kind = auth.get('kind', 'short')
                entry_size = 3 if kind == 'long' else 2
                entries = []
                tbl_pc = base
                for _i in range(count):
                    if tbl_pc + entry_size - 1 > 0xFFFF:
                        break
                    try:
                        tbl_off = lorom_offset(bank, tbl_pc)
                    except AssertionError:
                        break
                    if tbl_off + entry_size - 1 >= len(rom):
                        break
                    addr16 = rom[tbl_off] | (rom[tbl_off + 1] << 8)
                    if kind == 'long':
                        eb = rom[tbl_off + 2]
                        entries.append((eb << 16) | addr16)
                    else:
                        entries.append(addr16)
                    tbl_pc += entry_size
                insn.dispatch_entries = entries
                insn.dispatch_kind = kind
                # Fall-through edge IS preserved for an authorised JSR
                # (the call returns to the next insn, like any JSR).
                # Table entries are added as decode successors (jump
                # edges) so handlers get auto-promoted.
                labeled_succ = _labeled_successors(
                    insn, key, bank,
                    callee_exit_mx=callee_exit_mx,
                    callee_exit_mx_modes=callee_exit_mx_modes,
                    rom=rom, inline_arg_map=inline_arg_map,
                    stop_on_unknown_callee_exit=(
                        stop_on_unknown_callee_exit),
                    unknown_callee_exit_sites=(
                        graph.unknown_callee_exit_sites))
                # Append jump-kind edges to the in-bank handlers. Each
                # dispatch target enters as its own function — empty
                # p_stack, not the caller's.
                site_m = insn.m_flag & 1
                site_x = insn.x_flag & 1
                for e in entries:
                    e16 = e & 0xFFFF
                    eb = (e >> 16) & 0xFF if kind == 'long' else bank
                    if eb == bank and 0x8000 <= e16 <= 0xFFFF:
                        labeled_succ.append(
                            (DecodeKey(addr24(eb, e16), site_m, site_x, ()), 'jump')
                        )
                succ = [k for (k, _) in labeled_succ]
                graph.insns[key] = DecodedInsn(key=key, insn=insn, successors=succ)
                for s, sk in labeled_succ:
                    if s not in graph.insns:
                        worklist.append((s, sk, pc))
                continue
            # Runtime-pointer dispatch recovery (2026-06-21): a reachable
            # JSR (abs,X) whose pointer-table base is in WRAM/DP/low-RAM
            # ($0000-$1FFF) is a genuine per-object runtime function-pointer
            # dispatch — SM's enemy/PLM/eproj instruction-list interpreters
            # call `JSR ($0FA8/$0FAE/$0FB0/$0FB2,X)` where $0FAx holds a
            # per-object handler pointer written at runtime. The target is a
            # WRAM value, so it CANNOT be statically enumerated (the
            # `indirect_dispatch ... ptrcall targets:` form does not apply).
            # Route it through the runtime dispatcher (cpu_dispatch_call_pc):
            # read the pointer + dispatch the live (m,x) variant at run time,
            # AOT body if present else interpreter tier. The fall-through IS
            # preserved (a JSR returns to the next instruction).
            #
            # Phantom JSR (abs,X) decoded from garbage past an RTS have
            # ROM-range operands (>= $2000 — e.g. the $EA1D phantom pinned by
            # test_decoder_smc_phantom_suppression), so they stay SUPPRESSED
            # below. A WRAM-range operand is the discriminator: a function-
            # pointer table never lives in PPU/APU registers ($2000-$5FFF)
            # or ROM ($8000+); it lives in WRAM.
            if (insn.operand & 0xFFFF) < 0x2000:
                insn.dispatch_runtime = True
                insn.dispatch_idx_reg = 'X'
                labeled_succ = _labeled_successors(
                    insn, key, bank,
                    callee_exit_mx=callee_exit_mx,
                    callee_exit_mx_modes=callee_exit_mx_modes,
                    rom=rom, inline_arg_map=inline_arg_map,
                    stop_on_unknown_callee_exit=(
                        stop_on_unknown_callee_exit),
                    unknown_callee_exit_sites=(
                        graph.unknown_callee_exit_sites))
                succ = [k for (k, _) in labeled_succ]
                graph.insns[key] = DecodedInsn(key=key, insn=insn,
                                               successors=succ)
                for s, sk in labeled_succ:
                    if s not in graph.insns:
                        worklist.append((s, sk, pc))
                continue
            # UNAUTHORISED: drop fall-through; record for build report.
            # The insn lives in the graph (so predecessors' successor
            # edges still resolve) but with no outgoing successors.
            graph.insns[key] = DecodedInsn(key=key, insn=insn, successors=[])
            graph.suppressed_indirect_calls.append(SuppressedIndirectCall(
                site_pc24=site_pc24,
                table_base=insn.operand & 0xFFFF,
                function_entry_pc24=addr24(bank, start),
                entry_m=key.m,
                entry_x=key.x,
            ))
            continue

        labeled_succ = _labeled_successors(
            insn, key, bank,
            callee_exit_mx=callee_exit_mx,
            callee_exit_mx_modes=callee_exit_mx_modes,
            rom=rom, inline_arg_map=inline_arg_map,
            stop_on_unknown_callee_exit=stop_on_unknown_callee_exit,
            unknown_callee_exit_sites=graph.unknown_callee_exit_sites)
        succ = [k for (k, _) in labeled_succ]
        graph.insns[key] = DecodedInsn(key=key, insn=insn, successors=succ)

        for s, sk in labeled_succ:
            if s not in graph.insns:
                worklist.append((s, sk, pc))

    # PHP/PLP tracking causes the decoder to produce multiple DecodeKey
    # variants at the same (pc, m, x) when different p_stack histories
    # reach the same PC. The downstream IR + codegen identify blocks by
    # (pc, m, x) only (see emit_function._label_for), so multiple keys
    # at the same (pc, m, x) collide at C-label emission. Merge those
    # duplicates here: keep ONE representative DecodedInsn per (pc, m,
    # x), with the union of successors. Successor keys themselves are
    # remapped to the canonical key at each (pc, m, x).
    #
    # The merge preserves PHP/PLP correctness for the common case (one
    # bracket → one p_stack value reaches each PC) and degrades
    # gracefully for nested PHP/PLP (multiple variants at PLP produce
    # multiple successor (m, x) — the codegen emits each as a separate
    # downstream block).
    _dedupe_by_pcmx(graph)

    # Constant-Z branch fold + reachability prune. Runs once after the
    # worklist drains so predecessor counts are stable. See
    # `_apply_constant_z_fold` for the narrow scope.
    _apply_constant_z_fold(graph)

    return graph


def _dedupe_by_pcmx(graph: 'FunctionDecodeGraph') -> None:
    """Collapse DecodeKeys at the same (pc, m, x) — different p_stack —
    into one canonical key. Used by `decode_function` post-pass.

    Without dedupe, the gen-time _label_for(key) — which only uses
    (pc, m, x) — produces duplicate C labels when multiple p_stack
    histories reach the same PC + (m, x). The C compiler rejects with
    `error C2045: 'L_xxxx_MyXz': label redefined`.

    The merge keeps ONE DecodedInsn per (pc, m, x). Successors from
    all merged variants are unioned and themselves remapped to canonical
    keys.
    """
    canonical: Dict[Tuple[int, int, int], DecodeKey] = {}
    remap: Dict[DecodeKey, DecodeKey] = {}

    # Pass 1: pick canonical key per (pc, m, x). First-encountered wins.
    for key in graph.insns:
        pcmx = (key.pc, key.m, key.x)
        if pcmx not in canonical:
            canonical[pcmx] = key
        remap[key] = canonical[pcmx]

    # Pass 2: rebuild graph.insns with canonical keys + merged successors.
    #
    # IMPORTANT: deduplicate successors only ACROSS different DecodedInsn
    # variants at the same (pc, m, x), NOT within a single variant's
    # successors list. _labeled_successors emits (fall, jump) pairs for
    # conditional branches; when fall and jump point at the same target
    # (e.g. BRA offset 0), the duplicate must be preserved so that
    # downstream passes seeing `len(successors) == 2` (like the
    # constant-Z fold) still recognise the conditional shape.
    merged: Dict[DecodeKey, DecodedInsn] = {}
    seen_succ_per_canonical: Dict[DecodeKey, set] = {}
    for key, di in graph.insns.items():
        ck = remap[key]
        if ck not in merged:
            # First variant we see for this canonical key: take its
            # successors verbatim (duplicates intact), starting fresh.
            remapped_first = [remap.get(s, s) for s in di.successors]
            merged[ck] = DecodedInsn(key=ck, insn=di.insn,
                                     successors=remapped_first)
            seen_succ_per_canonical[ck] = set(remapped_first)
            continue
        # Subsequent variants at the same canonical (pc, m, x): append
        # only successors not already present in the merged successor
        # set. (We only see additional successors from variants reaching
        # this PC under a different p_stack — the per-variant successor
        # set was constructed by _labeled_successors already with the
        # right (fall, jump) duplication rules.)
        for s in di.successors:
            ms = remap.get(s, s)
            if ms not in seen_succ_per_canonical[ck]:
                merged[ck].successors.append(ms)
                seen_succ_per_canonical[ck].add(ms)

    graph.insns = merged

    # Remap the entry key in case the entry itself had a non-canonical
    # variant (unusual but possible if the entry has nonempty p_stack).
    if graph.entry in remap:
        graph.entry = remap[graph.entry]


def _direct_tail_exit_keys(graph: 'FunctionDecodeGraph',
                           decoded: 'DecodedInsn') -> List[Tuple[int, int, int]]:
    """Return exact entry keys reached by a direct tail transfer.

    A cfg sibling boundary deliberately leaves the successor outside this
    graph. Its callee exit is therefore also this function's exit. Long JMP
    has no decoder successor, so derive that target from its operand.
    """
    ins = decoded.insn
    if ins.mnem not in ('JMP', 'BRA', 'BRL'):
        return []
    outside = [
        (successor.pc & 0xFFFFFF, successor.m & 1, successor.x & 1)
        for successor in decoded.successors
        if successor not in graph.insns
    ]
    if outside:
        return outside
    if (ins.mnem == 'JMP' and ins.length == 4
            and not getattr(ins, 'dispatch_entries', None)):
        return [(ins.operand & 0xFFFFFF,
                 ins.m_flag & 1, ins.x_flag & 1)]
    return []


def _lookup_exit_mx(callee_exit_mx: Optional[Dict],
                    pc24: int, m: int, x: int):
    if callee_exit_mx is None:
        return None
    hit = callee_exit_mx.get((pc24 & 0xFFFFFF, m & 1, x & 1))
    if hit is None:
        bank = (pc24 >> 16) & 0xFF
        if bank < 0x40 or 0x80 <= bank < 0xC0:
            hit = callee_exit_mx.get(
                ((pc24 ^ 0x800000) & 0xFFFFFF, m & 1, x & 1))
    return hit


def analyze_function_exit_mx_modes(graph: 'FunctionDecodeGraph',
                                   callee_exit_mx: Optional[Dict] = None,
                                   ) -> Optional[Set[Tuple[int, int]]]:
    """Return the concrete set of (m, x) states at function exits.

    Returns None when an exit goes through a dispatch terminator whose
    handler exit state is not available yet. A set with more than one
    element is intentionally preserved for callers that need to decode
    post-call bytes under every possible width state.
    """
    modes: Set[Tuple[int, int]] = set()

    for di in graph.insns.values():
        ins = di.insn
        if ins.mnem in ('RTS', 'RTL', 'RTI'):
            modes.add((ins.m_flag & 1, ins.x_flag & 1))
            continue
        tail_keys = _direct_tail_exit_keys(graph, di)
        if tail_keys:
            for target, site_m, site_x in tail_keys:
                tail_exit = _lookup_exit_mx(
                    callee_exit_mx, target, site_m, site_x)
                if tail_exit is None:
                    return None
                modes.add((tail_exit[0] & 1, tail_exit[1] & 1))
            continue
        is_dispatch_term = (
            getattr(ins, 'dispatch_entries', None) is not None
            and len(di.successors) == 0
            and ins.mnem in ('JSL', 'JMP')
        )
        if is_dispatch_term:
            if callee_exit_mx is None:
                return None
            site_m = ins.m_flag & 1
            site_x = ins.x_flag & 1
            dispatcher_bank = (ins.addr >> 16) & 0xFF
            kind = getattr(ins, 'dispatch_kind', None)
            for entry in (ins.dispatch_entries or ()):
                if entry == 0:
                    continue
                if kind == 'long':
                    tgt_pc24 = entry & 0xFFFFFF
                else:
                    tgt_pc24 = (dispatcher_bank << 16) | (entry & 0xFFFF)
                handler_exit = callee_exit_mx.get((tgt_pc24, site_m, site_x))
                if handler_exit is None:
                    return None
                hm, hx = handler_exit
                if hm is None or hx is None:
                    return None
                modes.add((hm & 1, hx & 1))

    return modes if modes else None


def analyze_function_exit_mx(graph: 'FunctionDecodeGraph',
                             callee_exit_mx: Optional[Dict] = None,
                             ) -> 'Tuple[Optional[int], Optional[int]]':
    """Compute the (m, x) state at which a function returns to its caller.

    Walks every terminator in `graph` and takes the meet of the (m, x)
    state at which control leaves the function:

      - RTS/RTL/RTI: don't modify M/X, so each terminator's
        `(insn.m_flag, insn.x_flag)` IS the (m, x) at the moment of
        return.

      - JSL/JML dispatch terminator (`insn.dispatch_entries` populated
        and no fall-through successors): the dispatcher transfers
        control to a handler which RTLs back to OUR caller. The
        effective exit state at this terminator is whichever (m, x)
        the dispatched handler RTLs with. Requires `callee_exit_mx`
        to have an entry for each table target keyed by the dispatch
        site's (m, x); if any handler's exit is unknown we return
        `(None, None)` (retry on a later auto-router pass once more
        callees converge).

        Soundness note: the JSL dispatch helper itself runs in a
        canonical state (e.g. SMW's `$00:86FA` runs with the
        dispatcher's `(m, x)` and forwards without restoring P), so
        the handler is entered with the dispatch SITE's (m, x). Each
        handler's `callee_exit_mx[(target, site_m, site_x)]` IS the
        handler's RTL (m, x), which becomes our function's effective
        exit.

    If all return paths exit with the same (m, x), returns that pair.
    If any two return paths disagree, the corresponding component is
    `None` (ambiguous — the caller's decoder should fall back to its
    pre-call assumption rather than commit to a wrong width).

    Functions with no terminators (e.g. infinite loops, table-only)
    return `(None, None)` — no callable resume state to propagate.
    """
    exit_m: Optional[int] = None
    exit_x: Optional[int] = None
    have_any = False
    m_ambig = False
    x_ambig = False

    def _accumulate(em: int, ex: int) -> None:
        nonlocal exit_m, exit_x, have_any, m_ambig, x_ambig
        if not have_any:
            exit_m, exit_x = em, ex
            have_any = True
            return
        if not m_ambig and exit_m != em:
            m_ambig = True
        if not x_ambig and exit_x != ex:
            x_ambig = True

    for di in graph.insns.values():
        ins = di.insn
        if ins.mnem in ('RTS', 'RTL', 'RTI'):
            _accumulate(ins.m_flag & 1, ins.x_flag & 1)
            continue
        tail_keys = _direct_tail_exit_keys(graph, di)
        if tail_keys:
            for target, site_m, site_x in tail_keys:
                tail_exit = _lookup_exit_mx(
                    callee_exit_mx, target, site_m, site_x)
                if tail_exit is None:
                    return (None, None)
                _accumulate(tail_exit[0] & 1, tail_exit[1] & 1)
            continue
        # Dispatch terminator: JSL/JML with no successors and a
        # populated dispatch table. The function transfers control to
        # a handler that eventually RTLs back to our caller.
        is_dispatch_term = (
            getattr(ins, 'dispatch_entries', None) is not None
            and len(di.successors) == 0
            and ins.mnem in ('JSL', 'JMP')  # JMP here means JML (length 4)
        )
        if is_dispatch_term:
            if callee_exit_mx is None:
                # No callee-exit info → can't propagate handler exits.
                # Return ambiguous; auto-router will skip this entry
                # variant entirely, which is the safe default.
                return (None, None)
            site_m = ins.m_flag & 1
            site_x = ins.x_flag & 1
            dispatcher_bank = (ins.addr >> 16) & 0xFF
            kind = getattr(ins, 'dispatch_kind', None)
            for entry in (ins.dispatch_entries or ()):
                # Padding entries (0) are recorded by the decoder for
                # short and long tables; skip them.
                if entry == 0:
                    continue
                if kind == 'long':
                    tgt_pc24 = entry & 0xFFFFFF
                else:
                    # short: 16-bit target in dispatcher's bank.
                    tgt_pc24 = (dispatcher_bank << 16) | (entry & 0xFFFF)
                key = (tgt_pc24, site_m, site_x)
                handler_exit = callee_exit_mx.get(key)
                if handler_exit is None:
                    # Handler's exit at this site-(m, x) not yet known.
                    # Defer — a later auto-router iteration may resolve
                    # the chain. Stay ambiguous for now.
                    return (None, None)
                _accumulate(handler_exit[0] & 1, handler_exit[1] & 1)
            continue

    if m_ambig:
        exit_m = None
    if x_ambig:
        exit_x = None
    if not have_any:
        return (None, None)
    return (exit_m, exit_x)


def _apply_constant_z_fold(graph: FunctionDecodeGraph) -> None:
    """Decoder post-pass: rewrite BEQ/BNE successors to a single live
    edge when the same-block predecessor is an immediate LDA/LDX/LDY
    that makes Z statically known.

    Narrow scope (deliberate — see project_constant_z_fold spec):
        * Predecessor must be LDA/LDX/LDY in IMM addressing mode.
        * Predecessor's only successor must be this branch (no other
          edge can land on the load between it and the branch).
        * Branch must have exactly ONE predecessor (the load) and
          exactly TWO successors (fall + jump from _labeled_successors).
        * Op width follows m for LDA, x for LDX/LDY (entry mode of
          the load, which is what the decoder used to read its bytes).
        * Only Z-flag branches (BEQ/BNE). N/V/C are explicitly out of
          scope for this initial fold; SEP/REP/PLP/ALU are out too.

    On match:
        * graph.insns[branch_key].successors becomes [live_edge_only].
        * insn.const_z_fold_unconditional is set so lowering emits a
          `Goto` IR op (single successor) rather than a `CondBranch`
          (two-successor flag test).
        * insn.const_z_fold_dead_pc24 records the pruned target for
          the build report.
        * Reachability is recomputed from graph.entry; insns reachable
          ONLY through the pruned edge are removed from graph.insns
          (and therefore from cfg block construction + codegen). Their
          unresolvable-goto markers, if any, vanish along with them —
          that is the point of the fold.
        * graph.const_z_folds gets a record for the build report.
    """
    if not graph.insns:
        return

    # Build predecessors map.
    preds: Dict[DecodeKey, set] = {}
    for k, di in graph.insns.items():
        for s in di.successors:
            preds.setdefault(s, set()).add(k)

    # Apply folds. Iterate over a snapshot of keys because we mutate
    # graph.insns mid-loop.
    for k in list(graph.insns.keys()):
        di = graph.insns.get(k)
        if di is None:
            continue
        insn = di.insn
        if insn.mnem not in ('BEQ', 'BNE'):
            continue
        my_preds = preds.get(k, set())
        if len(my_preds) != 1:
            continue
        pred_key = next(iter(my_preds))
        pred_di = graph.insns.get(pred_key)
        if pred_di is None:
            continue
        pred_insn = pred_di.insn
        if pred_insn.mnem not in ('LDA', 'LDX', 'LDY'):
            continue
        if pred_insn.mode != IMM:
            continue
        if len(pred_di.successors) != 1 or pred_di.successors[0] != k:
            continue
        if len(di.successors) != 2:
            # Already pruned (defensive — shouldn't reach this branch).
            continue

        # Compute Z from the masked immediate. LDA uses m-width;
        # LDX/LDY use x-width. Use the LOAD's entry flags (the mode
        # under which decode_insn read its operand bytes).
        if pred_insn.mnem == 'LDA':
            width_bits = 8 if pred_insn.m_flag == 1 else 16
        else:
            width_bits = 8 if pred_insn.x_flag == 1 else 16
        mask = (1 << width_bits) - 1
        masked = pred_insn.operand & mask
        z = 1 if masked == 0 else 0

        # successors order from _labeled_successors for cond branch:
        # [(fall, 'fall'), (jump, 'jump')].
        fall_succ, jump_succ = di.successors[0], di.successors[1]
        if insn.mnem == 'BEQ':
            taken = (z == 1)
        else:  # BNE
            taken = (z == 0)
        live = jump_succ if taken else fall_succ
        dead = fall_succ if taken else jump_succ

        # Rewrite successors to single live edge.
        graph.insns[k] = DecodedInsn(key=k, insn=insn, successors=[live])
        insn.const_z_fold_unconditional = True
        insn.const_z_fold_dead_pc24 = dead.pc & 0xFFFFFF

        # Build a context-rich record for the report.
        graph.const_z_folds.append(ConstZFold(
            branch_pc24=insn.addr & 0xFFFFFF,
            prev_pc24=pred_insn.addr & 0xFFFFFF,
            branch_mnem=insn.mnem,
            prev_mnem=pred_insn.mnem,
            prev_imm=masked,
            width_bits=width_bits,
            z_value=z,
            taken_kind='jump' if taken else 'fall',
            live_pc24=live.pc & 0xFFFFFF,
            dead_pc24=dead.pc & 0xFFFFFF,
            func_entry_pc24=graph.entry.pc & 0xFFFFFF,
            entry_m=graph.entry.m & 1,
            entry_x=graph.entry.x & 1,
        ))

    # Reachability prune. Walk from entry; drop any insn no longer
    # reachable. Without this the dead-path insns linger in graph.insns
    # and cfg.build picks them up as orphan blocks (carrying any
    # unresolvable-goto markers they accumulated).
    reachable: set = set()
    work = [graph.entry]
    while work:
        cur = work.pop()
        if cur in reachable:
            continue
        if cur not in graph.insns:
            continue
        reachable.add(cur)
        for s in graph.insns[cur].successors:
            work.append(s)
    for k in list(graph.insns.keys()):
        if k not in reachable:
            del graph.insns[k]
