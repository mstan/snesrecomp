#!/usr/bin/env python3
"""
tools/recomp/recomp.py --65816 -> C static recompiler for snesrecomp-v2

Reads a SNES ROM, decodes 65816 instructions with full M/X flag tracking,
and emits C functions using the snesrev runtime API.

Architecture requirements (from HANDOFF.md):
  A. Full M/X flag tracking in decode AND emit
  B. Carry flag propagation across ALL ADC variants
  C. Register side effects from JSR returns (X update)
  D. Variable scoping with gotos (hoisted declarations)
  E. Direct page parameter passing
  F. 16-bit LDA/STA for wide mode

Usage:
    python recomp.py ROM.sfc CONFIG.cfg [-o OUT.c] [--trace]
    python recomp.py ROM.sfc --hexdump --bank 07 --addr F722 --len 32
    python recomp.py ROM.sfc --disasm  --bank 07 --addr F722 [--end F78A]
"""

import sys
import re
import argparse
import json
import os
from typing import Optional, List, Tuple, Dict, Set

from snes65816 import (
    load_rom, lorom_offset, rom_slice, decode_insn, validate_decoded_insns,
    Insn, _OPCODES, _build_opcode_table,
    IMP, ACC, IMM, DP, DP_X, DP_Y, ABS, ABS_X, ABS_Y,
    LONG, LONG_X, REL, REL16, STK, INDIR, INDIR_X, INDIR_Y, INDIR_LY,
    INDIR_L, INDIR_DPX, DP_INDIR, STK_IY, MODE_STR,
)
from discover import discover_bank

# Backward compat alias (internal uses of _validate_decoded_insns)
_validate_decoded_insns = validate_decoded_insns


def decode_func(rom: bytes, bank: int, start: int, end: int = 0,
                jsl_dispatch: Set[int] = None,
                jsl_dispatch_long: Set[int] = None,
                dispatch_known_addrs: Set[int] = None,
                mode_overrides: Dict[int, int] = None,
                validate_branches: bool = True,
                exclude_ranges: List[Tuple[int, int]] = None,
                known_func_starts: Set[int] = None) -> List[Insn]:
    """Decode instructions from start_addr until RTL/RTS or end address.

    Handles mid-body early returns by tracking unresolved forward branch targets.

    If validate_branches is True (default), the decoder speculatively follows
    out-of-range branches, validates the decoded instructions, and discards
    them if they look like data decoded as code.
    """
    insns = []
    pc = start
    m, x = 1, 1
    mode_overrides = mode_overrides or {}
    pending_fwd: Set[int] = set()
    pending_flags: Dict[int, Tuple[int,int]] = {}  # addr -> (m, x) from branch source
    decoded_pcs: Set[int] = set()

    extra_past_end = 0  # count of out-of-range targets followed
    max_insns = 2000  # safety limit
    max_extra = 32  # max out-of-range targets to follow
    _continuing_past_end = False  # True when following fall-through from past-end code
    while len(insns) < max_insns:
        # end_addr is exclusive (the first address NOT in the function). Stop
        # decoding once pc reaches it; otherwise decode_func pulls in the next
        # function's opening instruction, which downstream logic then
        # misidentifies as our terminator and suppresses fall-through emit.
        if end and pc >= end and not _continuing_past_end:
            if extra_past_end >= max_extra:
                break
            # Collect all out-of-range targets (backward and forward)
            out_of_range = {t for t in pending_fwd
                            if (t < start or t > end) and t not in decoded_pcs
                            and 0x8000 <= t <= 0xFFFF}
            # Filter out targets in exclude_ranges (known data)
            if exclude_ranges:
                out_of_range = {t for t in out_of_range
                                if not any(er_s <= t <= er_e for er_s, er_e in exclude_ranges)}
            # Filter out targets that are known function entries: those are
            # independent functions that should be tail-called at emit time,
            # not speculatively inlined into this function's insn list.
            if known_func_starts:
                out_of_range = {t for t in out_of_range
                                if ((bank << 16) | t) not in known_func_starts}
            # When end_addr is set, do not speculatively decode past it
            # either. The cfg author chose this boundary; respect it.
            if end:
                out_of_range = {t for t in out_of_range if t < end}
            if not out_of_range:
                break
            # Pick the nearest target to try
            target = min(out_of_range)
            # Speculative decode: try decoding from target, validate result
            if validate_branches:
                try:
                    spec_insns = []
                    spec_pc = target
                    spec_m = 1 if target < start else m
                    spec_x = 1 if target < start else x
                    for _ in range(16):
                        spec_off = lorom_offset(bank, spec_pc)
                        spec_insn = decode_insn(rom, spec_off, spec_pc, bank, spec_m, spec_x)
                        if spec_insn is None:
                            break
                        spec_insns.append(spec_insn)
                        if spec_insn.mnem == 'REP':
                            if spec_insn.operand & 0x20: spec_m = 0
                            if spec_insn.operand & 0x10: spec_x = 0
                        elif spec_insn.mnem == 'SEP':
                            if spec_insn.operand & 0x20: spec_m = 1
                            if spec_insn.operand & 0x10: spec_x = 1
                        if spec_insn.mnem in ('RTL', 'RTS', 'RTI', 'JMP'):
                            break
                        spec_pc += spec_insn.length
                    if not spec_insns or not _validate_decoded_insns(spec_insns, bank):
                        # Looks like data --discard, branch becomes a return
                        pending_fwd.discard(target)
                        extra_past_end += 1
                        continue
                except (AssertionError, IndexError):
                    pending_fwd.discard(target)
                    extra_past_end += 1
                    continue
            # Valid code --follow the target
            pc = target
            if target < start:
                m, x = 1, 1
            extra_past_end += 1
            _continuing_past_end = (end and target >= end)
            # Fall through to decode (don't continue)
        if pc in decoded_pcs:
            # Already decoded this address (can happen with backward branches)
            pending_fwd.discard(pc)
            if not pending_fwd:
                break
            next_target = min(pending_fwd)
            if next_target in decoded_pcs:
                pending_fwd.discard(next_target)
                if not pending_fwd:
                    break
                next_target = min(pending_fwd)
            pc = next_target
            # Restore flag state from the branch that targeted this address
            if pc in pending_flags:
                m, x = pending_flags[pc]
            continue
        decoded_pcs.add(pc)

        # Apply mode overrides before decoding
        if pc in mode_overrides:
            flags = mode_overrides[pc]
            if flags & 0x20: m = 0
            if flags & 0x10: x = 0
            # SEP overrides: bit 5 clear means force M=1, bit 4 clear means force X=1
            if not (flags & 0x20) and (flags & 0x40):  # explicit sep marker
                m = 1
            if not (flags & 0x10) and (flags & 0x40):
                x = 1

        if pc < 0x8000 or pc > 0xFFFF:
            break
        off = lorom_offset(bank, pc)
        insn = decode_insn(rom, off, pc, bank, m, x)
        if insn is None:
            print(f"  WARN: unknown opcode ${rom[off]:02X} at ${bank:02X}:{pc:04X}",
                  file=sys.stderr)
            break
        insn.m_flag = m
        insn.x_flag = x
        insns.append(insn)

        # Track M/X flag changes
        if insn.mnem == 'REP':
            if insn.operand & 0x20: m = 0
            if insn.operand & 0x10: x = 0
        elif insn.mnem == 'SEP':
            if insn.operand & 0x20: m = 1
            if insn.operand & 0x10: x = 1

        next_pc = pc + insn.length

        # JSL/JML inline dispatch table detection
        # JMP LONG (opcode $5C = JML) uses the same dispatch table pattern as JSL.
        _is_jsl_or_jml = insn.mnem == 'JSL' or (insn.mnem == 'JMP' and insn.mode == LONG)
        _is_short_disp = jsl_dispatch and _is_jsl_or_jml and insn.operand in jsl_dispatch
        _is_long_disp  = jsl_dispatch_long and _is_jsl_or_jml and insn.operand in jsl_dispatch_long
        if _is_short_disp or _is_long_disp:
            entries = []
            tbl_pc = next_pc
            entry_size = 3 if _is_long_disp else 2
            while len(entries) < 256 and tbl_pc + entry_size - 1 <= 0xFFFF:
                try:
                    tbl_off = lorom_offset(bank, tbl_pc)
                except AssertionError:
                    break
                lo = rom[tbl_off]
                hi = rom[tbl_off + 1] if tbl_off + 1 < len(rom) else 0
                if _is_long_disp:
                    entry_bank = rom[tbl_off + 2] if tbl_off + 2 < len(rom) else 0
                    entry = lo | (hi << 8)
                    # Null entries (all zero) are valid "no handler" slots
                    # in sparse dispatch tables; keep reading past them.
                    if entry == 0 and entry_bank == 0:
                        entries.append(0)
                        tbl_pc += entry_size
                        continue
                    if entry < 0x8000 or entry_bank != bank:
                        break
                    full_entry = (entry_bank << 16) | entry
                else:
                    entry = lo | (hi << 8)
                    if entry == 0:
                        entries.append(0)
                        tbl_pc += entry_size
                        continue
                    if entry < 0x8000:
                        break
                    full_entry = (bank << 16) | entry
                # Accept entries that are known OR within reasonable range.
                if dispatch_known_addrs and full_entry not in dispatch_known_addrs:
                    if exclude_ranges and any(er_s <= entry <= er_e for er_s, er_e in exclude_ranges):
                        break
                    # For configured jsl_dispatch targets, trust all $8000+
                    # entries bank-wide (the user declared this IS a dispatch).
                    # For auto-detected dispatches, use a tighter proximity check.
                    if not (_is_short_disp or _is_long_disp):
                        if abs(entry - pc) > 0x800:
                            break
                    # Dispatch-overread cap: if the entry is not a known function,
                    # require it to land inside the containing function's range
                    # [start, end). Entries outside the range are data the decoder
                    # read past the real table end (no cfg-level count hint exists).
                    # Known funcs (including sub-entries) bypass this check and can
                    # legitimately point anywhere in the bank.
                    if end and start and not (start <= entry < end):
                        break
                entries.append(entry)
                tbl_pc += entry_size
            if entries:
                insn.dispatch_entries = entries
                next_pc = tbl_pc
                # If ALL entries are known external functions the emitter will
                # emit a function-pointer table --no inline labels needed, and
                # the dispatch acts as a terminator (no fall-through decoding).
                # If ANY entry is unknown/internal the emitter falls back to
                # switch/goto and EVERY entry must be decoded as a label.
                # Null entries ($000000) are "no handler" — they don't
                # need to be known funcs; they become case:return; at emit.
                all_ext = dispatch_known_addrs and all(
                    e == 0 or (bank << 16) | e in dispatch_known_addrs
                    for e in entries)
                if all_ext:
                    insn.dispatch_terminal = True
                else:
                    for entry in entries:
                        if entry not in decoded_pcs:
                            pending_fwd.add(entry)

        # Track branch targets with flag state
        if insn.mnem in ('BPL','BMI','BEQ','BNE','BCC','BCS','BVS','BVC','BRA','BRL'):
            tgt = insn.operand
            if 0x8000 <= tgt <= 0xFFFF:
                if tgt not in decoded_pcs:
                    pending_fwd.add(tgt)
                # Always update flags — later branches to the same target override
                # earlier ones. This handles convergence: the last branch to set
                # flags wins (typically the fall-through from a SEP/REP).
                pending_flags[tgt] = (m, x)
        elif insn.mnem == 'JMP' and insn.mode == ABS:
            tgt = insn.operand
            if 0x8000 <= tgt <= 0xFFFF:
                full_tgt = (bank << 16) | tgt
                if full_tgt not in (dispatch_known_addrs or set()):
                    pending_fwd.add(tgt)
                pending_flags[tgt] = (m, x)

        pending_fwd.discard(pc)

        is_return = insn.mnem in ('RTL', 'RTS', 'RTI')
        is_uncond_jmp = (insn.mnem == 'JMP' and insn.mode in (ABS, LONG, INDIR, INDIR_X))
        is_uncond_branch = insn.mnem in ('BRA', 'BRL')
        if is_return or is_uncond_jmp or is_uncond_branch or insn.dispatch_terminal:
            _continuing_past_end = False
            # Drop pending targets that are known function entries outside
            # this function — they are independent functions, emit-time will
            # tail-call them.
            if known_func_starts:
                pending_fwd = {t for t in pending_fwd
                               if ((bank << 16) | t) not in known_func_starts
                               or (start <= t and (not end or t < end))}
            # When end_addr is set, do not chase pending targets past it.
            # Branches to past-end addresses (e.g. shared RTS in a sibling
            # function) are handled at emit time via tail-call /
            # branch-as-return. Decoding past end inlines the sibling
            # function's body, which corrupts both functions.
            if end:
                pending_fwd = {t for t in pending_fwd if t < end}
            if not pending_fwd:
                break
            # Skip to lowest unresolved forward branch target --avoids
            # decoding inline data between a terminator and the target.
            pc = min(pending_fwd)
            # If jumping to a past-end target that the function branched to,
            # enter continuation mode to decode fall-through there too.
            if end and pc >= end:
                _continuing_past_end = True
            # Restore flag state from the branch that targeted this address
            if pc in pending_flags:
                m, x = pending_flags[pc]
            continue

        # Do NOT enable continuation mode here on natural fall-through.
        # _continuing_past_end is set only via legitimate paths:
        #   - line 120: chased an out_of_range branch target past end
        #   - line 262: post-terminator pending_fwd pointed past end
        # Setting it unconditionally when pc >= end would cause decoding
        # to run unbounded past end_addr whenever the function ends on a
        # non-terminal instruction (breaks natural-fall-through boundaries
        # like GameMode03Entry -> $96CF).
        pc = next_pc
    return insns

# ==============================================================================
# SIGNATURE PARSING
# ==============================================================================

# Primitive types cfg's AUTO analysis can derive. Anything else (structs,
# pointers, bool, RetAY) is semantic info only funcs.h carries.
_SIMPLE_TYPES = frozenset(('void', 'uint8', 'uint16', 'int8', 'int16'))


def _sig_has_complex_type(sig: Optional[str]) -> bool:
    """True if sig declares any non-primitive type (return or param)."""
    if not sig:
        return False
    ret, params = parse_sig(sig)
    if ret not in _SIMPLE_TYPES:
        return True
    for ptype, _pname in params:
        if ptype not in _SIMPLE_TYPES:
            return True
    return False


def _ret_is_pointer(sig: Optional[str]) -> bool:
    """True if the sig's return type is a pointer (e.g. 'uint8*').

    Pointer returns are a SNES-code oddity: the ROM typically communicates
    the pointer via DP writes, not via the A register. The recompiler's A
    tracking can't carry a pointer value through arithmetic without
    breaking subsequent byte-level usage (e.g. ROL patterns). We therefore
    keep the body sig as void when cfg says void, even if funcs.h declares
    a pointer return — the callers that DO consume the pointer go through
    hand-written / oracle code paths, not generated arithmetic.
    """
    if not sig:
        return False
    ret, _ = parse_sig(sig)
    return '*' in ret


def _sig_specificity(sig: Optional[str]) -> Tuple[int, int, int]:
    """Specificity score for picking the better of two sigs.
    (complex_types, param_count, non_void_return). Higher wins.

    - complex_types: count of non-primitive types (struct, ptr-to-struct, bool,
      RetAY) across return + params. These encode semantics the recompiler's
      AUTO analysis cannot derive, so they should dominate.
    - param_count: a sig declaring `void(uint8_k)` is strictly more
      informative than `void()` — the caller's register-passing convention
      depends on it.
    - non_void_return: break ties in favor of sigs that actually return
      a value (cfg AUTO sometimes misses the A-in-RTS return pattern).

    Sigs that tie on all three are considered equally specific; callers
    should keep whichever they already had (typically the defining bank's
    entry, which is processed first).
    """
    if not sig:
        return (0, 0, 0)
    ret, params = parse_sig(sig)
    complex_types = 1 if ret not in _SIMPLE_TYPES else 0
    for ptype, _pname in params:
        if ptype not in _SIMPLE_TYPES:
            complex_types += 1
    non_void = 1 if ret != 'void' else 0
    return (complex_types, len(params), non_void)


def _reconcile_sig(cfg_sig: Optional[str], funcs_h_sig: Optional[str]) -> Optional[str]:
    """Pick a single sig used for BOTH C declaration and body codegen.

    Picks the sig with higher specificity (see _sig_specificity). This
    favors complex return types from funcs.h (RetAY, struct, ptr) and
    favors whichever source declares more parameter information, so callers
    and callees agree on the register-passing convention.

    Exception: when funcs.h declares a POINTER return and cfg says void,
    the cfg's void wins for codegen purposes — see _ret_is_pointer. The
    declared return type is preserved via a separate decl_ret_override map
    in run_config (keeps oracle callers happy; the body's lack of a real
    return becomes a C4715 warning rather than a C2297 compile error).
    """
    if not funcs_h_sig:
        return cfg_sig
    if not cfg_sig:
        return funcs_h_sig
    if _ret_is_pointer(funcs_h_sig) and parse_sig(cfg_sig)[0] == 'void':
        return cfg_sig
    if _sig_specificity(funcs_h_sig) > _sig_specificity(cfg_sig):
        return funcs_h_sig
    return cfg_sig


# ==============================================================================
# LIVE-IN REGISTER INFERENCE
# ==============================================================================
#
# For any function whose entry basic block reads A, X, or Y before writing it,
# that register is an input parameter by definition — the caller must supply
# it. The cfg cannot declare this for every function (and shouldn't need to):
# it's fully derivable from the ROM. This pass walks the decoded instruction
# graph from the function entry and reports which registers are live-in, along
# with the m/x width at the first read site.
#
# Rule 0 applies: the recompiler derives sigs from the ROM; cfg only overrides
# for information that cannot be derived (struct returns, DP-passed params,
# non-register calling conventions).

# Mnemonics that read a register implicitly (before any write). Read+write
# mnems (e.g. ADC, INC A) are listed here too — the read happens first.
#
# Note: PHA/PHX/PHY are intentionally OMITTED. In 65816 calling conventions
# the dominant pattern is "PH{A,X,Y} at entry, PL{A,X,Y} at exit" to save
# and restore a register the callee wants to scribble on. That push reads
# the register, but semantically the caller doesn't need to supply a
# meaningful value — the function is just preserving whatever was there.
# Counting PH* as a liveness read would promote every save-restore helper
# into spuriously taking a register parameter, which breaks verbatim
# callers that rightly call such helpers with no arguments.
_A_IMPLICIT_READERS = frozenset({
    'STA',
    'TAX', 'TAY', 'TCD', 'TCS',
    'AND', 'ORA', 'EOR', 'ADC', 'SBC', 'CMP', 'BIT',
    'MVN', 'MVP',
    'XBA',  # swaps A<->B: reads both
})
_A_IMPLICIT_WRITERS = frozenset({
    'LDA', 'PLA',
    'TXA', 'TYA', 'TDC', 'TSC',
    'AND', 'ORA', 'EOR', 'ADC', 'SBC',
    'MVN', 'MVP',
    'XBA',
})

_X_IMPLICIT_READERS = frozenset({
    'STX',
    'TXA', 'TXS', 'TXY',
    'CPX',
    'DEX', 'INX',
    'MVN', 'MVP',
})
_X_IMPLICIT_WRITERS = frozenset({
    'LDX', 'PLX',
    'TAX', 'TSX', 'TYX',
    'DEX', 'INX',
    'MVN', 'MVP',
})

_Y_IMPLICIT_READERS = frozenset({
    'STY',
    'TYA', 'TYX',
    'CPY',
    'DEY', 'INY',
    'MVN', 'MVP',
})
_Y_IMPLICIT_WRITERS = frozenset({
    'LDY', 'PLY',
    'TAY', 'TXY',
    'DEY', 'INY',
    'MVN', 'MVP',
})

# Addressing modes that use an index register.
_X_INDEX_MODES = frozenset({ABS_X, DP_X, LONG_X, INDIR_X, INDIR_DPX})
_Y_INDEX_MODES = frozenset({ABS_Y, DP_Y, INDIR_Y, INDIR_LY, STK_IY})

# A-accumulator shift/rotate/inc/dec read+write A when mode is ACC.
_ACC_MODE_RW_MNEMS = frozenset({'ASL', 'LSR', 'ROL', 'ROR', 'INC', 'DEC'})


def _insn_reg_use(insn: Insn, reg: str) -> Tuple[bool, bool]:
    """Return (reads, writes) flags for `reg` ('A' | 'X' | 'Y') on this insn.

    Read+write mnemonics (e.g. ADC, INC A) report both True — the read
    happens before the write, so for live-in detection the read wins.
    """
    mn = insn.mnem
    mode = insn.mode
    reads = False
    writes = False
    if reg == 'A':
        if mn in _A_IMPLICIT_READERS:
            reads = True
        if mn in _A_IMPLICIT_WRITERS:
            writes = True
        if mode == ACC and mn in _ACC_MODE_RW_MNEMS:
            reads = True
            writes = True
    elif reg == 'X':
        if mn in _X_IMPLICIT_READERS:
            reads = True
        if mn in _X_IMPLICIT_WRITERS:
            writes = True
        if mode in _X_INDEX_MODES:
            reads = True
    elif reg == 'Y':
        if mn in _Y_IMPLICIT_READERS:
            reads = True
        if mn in _Y_IMPLICIT_WRITERS:
            writes = True
        if mode in _Y_INDEX_MODES:
            reads = True
    return reads, writes


def infer_live_in_regs(insns: List[Insn], start_addr: int,
                       bank: int = 0,
                       callee_sigs: Optional[Dict[int, str]] = None,
                       callee_clobbers: Optional[Dict[int, Set[str]]] = None) -> Dict[str, int]:
    """Compute which of A, X, Y are live-in at function entry.

    Walks the in-function instruction graph from `start_addr`, asking for
    each register: is there a reachable read before any write? If so, the
    register is a parameter, and the width is taken from the m/x flag on
    the insn that first reads it (m=0 -> 16-bit A; x=0 -> 16-bit X/Y).

    Returns a dict like {'A': 8, 'X': None, 'Y': 16}. The value is the bit
    width when live-in, or None when not live-in.

    Conservative assumptions:
      - JSR/JSL to a callee with a known sig: any register the callee
        takes as a parameter is treated as READ by this call (the caller
        must supply it), THEN the call's return-value-carrying registers
        become defined. Without this, trampoline functions like
        `JSR $F465 ; RTS` wrap a callee that takes X but lose X-as-input
        because the JSR was treated as a pure def.
      - JSR/JSL with no known callee sig: conservatively define A, X, Y
        (callees may return a value in any of them).
      - RTS/RTL/RTI/BRK/STP terminate paths.
      - Unconditional JMP/BRA/BRL within the function follows the target;
        cross-function JMP (target not in decoded set) terminates the path.
      - Conditional branches fork. Both branches are explored.
    """
    callee_sigs = callee_sigs or {}
    callee_clobbers = callee_clobbers or {}
    if not insns:
        return {'A': None, 'X': None, 'Y': None}

    insn_by_addr = {i.addr & 0xFFFF: i for i in insns}
    sorted_addrs = sorted(insn_by_addr)
    if not sorted_addrs:
        return {'A': None, 'X': None, 'Y': None}
    addr_to_idx = {a: i for i, a in enumerate(sorted_addrs)}
    start16 = start_addr & 0xFFFF
    # If decode didn't start exactly at start_addr (rare: sub-entry), fall back
    # to the lowest decoded address.
    if start16 not in insn_by_addr:
        start16 = sorted_addrs[0]

    def _succs(addr: int) -> List[int]:
        insn = insn_by_addr.get(addr)
        if insn is None:
            return []
        mn = insn.mnem
        if mn in ('RTS', 'RTL', 'RTI', 'BRK', 'STP'):
            return []
        if mn in ('JMP', 'BRA', 'BRL'):
            if insn.mode == ABS and insn.operand in insn_by_addr:
                return [insn.operand]
            # Cross-function JMP, indirect JMP, JML — terminate path for
            # liveness purposes. (Tail calls don't bring A/X/Y back.)
            return []
        succs = []
        if mn in ('BPL','BMI','BEQ','BNE','BCC','BCS','BVS','BVC'):
            if insn.operand in insn_by_addr:
                succs.append(insn.operand)
        # Fall-through
        idx = addr_to_idx.get(addr)
        if idx is not None and idx + 1 < len(sorted_addrs):
            succs.append(sorted_addrs[idx + 1])
        return succs

    def _reg_live_in(reg: str) -> Optional[int]:
        # BFS with binary 'defined' state. Each (addr, defined) pair visited
        # at most once -> linear time.
        from collections import deque
        queue = deque([(start16, False)])
        visited = set()
        while queue:
            addr, defined = queue.popleft()
            key = (addr, defined)
            if key in visited:
                continue
            visited.add(key)
            insn = insn_by_addr.get(addr)
            if insn is None:
                continue
            reads, writes = _insn_reg_use(insn, reg)
            # BIT-for-V-flag idiom: `BIT abs ; BVS/BVC ...` reads A only
            # to compute the Z flag (A & mem == 0), but if the next
            # instruction is BVS/BVC, only the V flag (bit 6 of memory,
            # independent of A) is tested. The Z flag result is dead, so
            # the A-read is dead too. Drop it here so liveness doesn't
            # spuriously promote the function's sig to take `uint8 a`.
            # Narrow by design: we only match the literal `BIT ; BVS/BVC`
            # pair. BIT followed by BEQ/BNE keeps A-read live since Z
            # does depend on A.
            if reg == 'A' and insn.mnem == 'BIT' and reads:
                idx = addr_to_idx.get(addr)
                if idx is not None and idx + 1 < len(sorted_addrs):
                    next_insn = insn_by_addr.get(sorted_addrs[idx + 1])
                    if next_insn is not None and next_insn.mnem in ('BVS', 'BVC'):
                        reads = False
            # JSR/JSL/JMP/BRA/BRL (call or tail transfer): ask the target's
            # declared sig whether it consumes this register. If so, the
            # transfer is a READ of the register — any caller-side code
            # reaching this instruction without having written the register
            # is using an input value. Handles:
            #   * classic trampolines: JSR foo ; RTS
            #   * chained trampolines: JSR a ; JMP b
            #   * bare branch tail calls: STA $xx ; BRA next_func (common
            #     pattern for auto-promoted sub-entries that simply set a
            #     DP byte before flowing into the real handler).
            is_call = insn.mnem in ('JSR', 'JSL')
            is_tail_transfer = (
                (insn.mnem == 'JMP' and insn.mode in (ABS, LONG)) or
                (insn.mnem in ('BRA', 'BRL',
                               'BPL','BMI','BEQ','BNE',
                               'BCC','BCS','BVS','BVC'))
            )
            # A branch to an intra-function target is a local goto, not a
            # tail call — skip those to avoid false-positive param reads
            # from label gotos.
            if is_tail_transfer and insn.operand in insn_by_addr:
                is_tail_transfer = False
            if is_call or is_tail_transfer:
                target = insn.operand
                if is_call and insn.mnem == 'JSR':
                    target = (bank << 16) | (target & 0xFFFF)
                elif is_tail_transfer and insn.mode in (ABS, REL, REL16):
                    target = (bank << 16) | (target & 0xFFFF)
                callee_sig = callee_sigs.get(target)
                if callee_sig:
                    _cret, cparams = parse_sig(callee_sig)
                    pnames = {pn for _pt, pn in cparams}
                    if ((reg == 'A' and 'a' in pnames)
                            or (reg == 'X' and 'k' in pnames)
                            or (reg == 'Y' and 'j' in pnames)):
                        reads = True
            if reads and not defined:
                # Width: A follows m flag, X/Y follow x flag.
                if reg == 'A':
                    return 16 if insn.m_flag == 0 else 8
                else:
                    return 16 if insn.x_flag == 0 else 8
            new_defined = defined or writes
            # JSR/JSL may return values in A/X/Y — treat as a def so that
            # post-call reads don't spuriously mark the reg as live-in.
            # EXCEPT: if we have clobber info saying the callee preserves
            # this register (not in callee_clobbers[target]), the call
            # doesn't re-define it. This lets caller-side liveness see
            # through preserve-X/preserve-Y helpers, so a post-JSR read
            # of Y correctly implies Y was live-in at function entry.
            if insn.mnem in ('JSR', 'JSL'):
                target = insn.operand
                if insn.mnem == 'JSR':
                    target = (bank << 16) | (target & 0xFFFF)
                clob = callee_clobbers.get(target)
                if clob is not None and reg not in clob:
                    # Callee is known to preserve this register.
                    pass
                else:
                    new_defined = True
            for succ in _succs(addr):
                queue.append((succ, new_defined))
        return None

    return {
        'A': _reg_live_in('A'),
        'X': _reg_live_in('X'),
        'Y': _reg_live_in('Y'),
    }


def _detect_register_restore_expr(insns: List[Insn], reg: str) -> Optional[str]:
    """Generalised version of _detect_x_restore_expr that works for any of
    A/X/Y. Returns the g_ram[0xXXXX] expression matching the most-recent
    LOAD from DP/ABS of `reg` before each RTS/RTL exit, when all exits
    agree. Returns None otherwise.
    """
    ld = {'A': 'LDA', 'X': 'LDX', 'Y': 'LDY'}[reg]
    if not insns:
        return None
    insn_by_addr = {i.addr & 0xFFFF: i for i in insns}
    sorted_addrs = sorted(insn_by_addr)
    idx = {a: i for i, a in enumerate(sorted_addrs)}
    restore_addrs: Set[int] = set()
    for insn in insns:
        if insn.mnem not in ('RTS', 'RTL'):
            continue
        i = idx[insn.addr & 0xFFFF]
        j = i - 1
        while j >= 0:
            cand = insn_by_addr[sorted_addrs[j]]
            _r, w = _insn_reg_use(cand, reg)
            if not w:
                j -= 1
                continue
            if cand.mnem == ld and cand.mode in (DP, ABS):
                restore_addrs.add(cand.operand)
            else:
                return None
            break
        else:
            continue
    if not restore_addrs or len(restore_addrs) > 1:
        return None
    addr = next(iter(restore_addrs))
    return f'g_ram[0x{addr:x}]'


def _detect_x_restore_expr(insns: List[Insn]) -> Optional[str]:
    """Detect an explicit X-restore pattern just before every RTS/RTL.

    The common SMW pattern:
        ...
        LDX  $xxxx      ; reload caller-visible X from WRAM
        RTS             ; return with X matching what the caller had

    When the instruction immediately before a terminal RTS/RTL is
    `LDX $xxxx` (direct-page or absolute), the callee is explicitly
    restoring X from a known WRAM location. Callers can treat the
    register as preserved *through* the call: their post-call X is
    whatever is at that WRAM address.

    Returns the C expression (`g_ram[0xXXXX]`) for the restore source,
    or None when no such pattern is visible. Falls back to None if
    multiple RTS exits restore from different addresses (inconsistent).

    (Symmetric `LDY $xxxx ; RTS` could drive a Y restore the same way,
    but the existing cfg schema only exposes restores_x today.)
    """
    if not insns:
        return None
    insn_by_addr = {i.addr & 0xFFFF: i for i in insns}
    sorted_addrs = sorted(insn_by_addr)
    idx = {a: i for i, a in enumerate(sorted_addrs)}
    restore_addrs: Set[int] = set()
    for insn in insns:
        if insn.mnem not in ('RTS', 'RTL'):
            continue
        i = idx[insn.addr & 0xFFFF]
        # Walk backwards from the RTS looking for the most-recent insn
        # that WRITES X (not just reads it via indexed addressing). Code
        # between the write and the RTS may use X via STA $xxxx,X etc.
        # without disturbing it. If the most-recent writer is an
        # LDX DP/ABS, that's our deterministic restore.
        j = i - 1
        while j >= 0:
            cand = insn_by_addr[sorted_addrs[j]]
            _r, w = _insn_reg_use(cand, 'X')
            if not w:
                j -= 1
                continue
            if cand.mnem == 'LDX' and cand.mode in (DP, ABS):
                restore_addrs.add(cand.operand)
            else:
                return None  # any other X-writer (TAX/TSX/TYX/PLX/INX/DEX)
                             # means X at RTS is not a deterministic WRAM
                             # load; bail so we don't pretend it is.
            break
        else:
            # No X write on this path — X is preserved "by not touching
            # it" which is already covered by the clobber check.
            continue
    if not restore_addrs or len(restore_addrs) > 1:
        return None
    addr = next(iter(restore_addrs))
    return f'g_ram[0x{addr:x}]'


def _looks_like_carry_return(insns: List[Insn]) -> bool:
    """Heuristic: the function only manipulates the carry flag (CLC/SEC)
    and returns without ever writing A. The SMW idiom for "bool return
    via carry" is:

        CLC          ; or SEC
        RTS

    Or a slightly longer variant where the function is a join point for
    multiple early-return branches that all ultimately just set or clear
    carry before returning. Such functions are best expressed as
    `carry_ret` so the emitter returns the carry expression instead of
    falling through to `return 0;` with an "A unknown at return" warning
    when the cfg happens to declare `uint8()` return.

    Returns True when every exit's preceding instruction is CLC or SEC
    and no A writer appears anywhere in the body.
    """
    if not insns:
        return False
    insn_by_addr = {i.addr & 0xFFFF: i for i in insns}
    sorted_addrs = sorted(insn_by_addr)
    idx = {a: i for i, a in enumerate(sorted_addrs)}
    # Any A writer disqualifies.
    for insn in insns:
        _r, w = _insn_reg_use(insn, 'A')
        if w:
            return False
    # Every RTS/RTL must be preceded by CLC or SEC on some path.
    for insn in insns:
        if insn.mnem not in ('RTS', 'RTL'):
            continue
        i = idx[insn.addr & 0xFFFF]
        # Walk backward looking for a carry-affecting instruction as the
        # most-recent operation. Skip pass-through ops (PLB/PHB/etc.)
        # that don't touch carry or A.
        found_carry = False
        j = i - 1
        while j >= 0:
            cand = insn_by_addr[sorted_addrs[j]]
            if cand.mnem in ('CLC', 'SEC'):
                found_carry = True
                break
            if cand.mnem in ('PLB', 'PHB', 'PLP', 'PHP', 'NOP', 'XCE'):
                j -= 1
                continue
            # Any other instruction invalidates the pure-carry pattern.
            break
        if not found_carry:
            return False
    return True


def _has_memory_save_restore(insns: List[Insn], reg: str) -> bool:
    """True if the function uses the STORE-at-entry / LOAD-before-exit
    idiom to preserve `reg`. Common alternative to PH{R}/PL{R}:

        STY  $03          ; save Y on entry
        ...arbitrary code that scribbles on Y...
        LDY  $03          ; restore Y before returning
        RTS (or JMP)

    Returns True when some early instruction is a store from `reg` to
    a DP/ABS address AND some later instruction loads `reg` back from
    the same address. Callers that are preserve-X/Y-aware can treat
    such a function as non-clobbering even though intermediate code
    modifies the register.
    """
    st_mnem = {'A': 'STA', 'X': 'STX', 'Y': 'STY'}[reg]
    ld_mnem = {'A': 'LDA', 'X': 'LDX', 'Y': 'LDY'}[reg]
    saved_addrs: Set[int] = set()
    for insn in insns:
        if insn.mnem == st_mnem and insn.mode in (DP, ABS):
            saved_addrs.add(insn.operand)
        if insn.mnem == ld_mnem and insn.mode in (DP, ABS):
            if insn.operand in saved_addrs:
                return True
    return False


def _writes_register_without_save_restore(insns: List[Insn], reg: str) -> bool:
    """True if the function writes `reg` anywhere in its body AND does not
    bracket those writes with a PH{R}/PL{R} save-restore at the function
    boundary. Used to detect callees that clobber A/X/Y from the caller's
    perspective, which in turn drives RetY/RetAY inference.

    Heuristic: find the first PH{R} (at function entry), find the last
    PL{R} before any RTS/RTL/RTI exit. If every write to `reg` is
    strictly between the PH{R} and the PL{R}, the register is preserved.
    Otherwise it's clobbered.

    Simplification: if there is no PH{R}/PL{R} pair at all and there is
    at least one write to `reg`, the register is clobbered. This catches
    the common SMW pattern where a small helper computes a Y return
    value with LDY #$xx / INY / ... / RTS and no save-restore.
    """
    ph = {'A': 'PHA', 'X': 'PHX', 'Y': 'PHY'}[reg]
    pl = {'A': 'PLA', 'X': 'PLX', 'Y': 'PLY'}[reg]
    has_write = False
    has_push = False
    has_pop = False
    for insn in insns:
        _r, w = _insn_reg_use(insn, reg)
        if w:
            has_write = True
        if insn.mnem == ph:
            has_push = True
        if insn.mnem == pl:
            has_pop = True
    if not has_write:
        return False
    # If both PH{R} and PL{R} are present, assume the function preserves
    # the register (save-restore pattern). This is an approximation —
    # strict analysis would verify the PH/PL pair brackets all writes and
    # lies on every path — but matches the common decomp convention.
    if has_push and has_pop:
        return False
    # Memory-based save-restore (STR $addr at entry, LDR $addr before
    # exit) is equivalent: the register's caller-visible value is
    # preserved via a WRAM slot even though the body scribbles on it.
    # Example: $00:86DF opens with `STY $03` and ends with `LDY $03`
    # right before `JMP (abs)`.
    if _has_memory_save_restore(insns, reg):
        return False
    # Explicit LD{R} $addr right before the last RTS is another
    # preserve signal — the body's final write to `reg` is a reload
    # from a deterministic WRAM slot, so callers can treat that slot
    # as the post-call register value (also captured by
    # _detect_register_restore_expr for X-restore wiring).
    if _detect_register_restore_expr(insns, reg) is not None:
        return False
    return True


def _augment_sig_with_livein(sig: Optional[str], live_in: Dict[str, Optional[int]]) -> Optional[str]:
    """Add any live-in register parameters missing from `sig`.

    Cfg/funcs.h sigs keep priority for types, struct returns, DP params, and
    any explicit `_a`/`_k`/`_j` params already declared. If inference finds
    A/X/Y live-in and the sig doesn't already pass it via a register param,
    one is appended.

    Convention (matches existing cfg usage):
      - A live-in -> append `uint8_a` / `uint16_a`.
      - X live-in -> append `uint8_k` / `uint16_k`.
      - Y live-in -> append `uint8_j` / `uint16_j`.

    Ordering within the argument list matches the order registers first
    become live, so callers using positional args stay consistent.
    """
    # When sig is None, treat it as a fresh `void()` — do NOT inherit
    # parse_sig's legacy default of `[('uint8','k')]`, which would inject a
    # phantom `k` parameter even when liveness analysis says X is not
    # live-in at entry.
    if sig is None:
        ret, params = 'void', []
    else:
        ret, params = parse_sig(sig)
    have_a = any(pname == 'a' for _pt, pname in params)
    have_x = any(pname in ('k',) for _pt, pname in params)
    have_y = any(pname == 'j' for _pt, pname in params)

    new_params = list(params)
    if live_in.get('X') is not None and not have_x:
        t = 'uint16' if live_in['X'] == 16 else 'uint8'
        new_params.append((t, 'k'))
    if live_in.get('Y') is not None and not have_y:
        t = 'uint16' if live_in['Y'] == 16 else 'uint8'
        new_params.append((t, 'j'))
    if live_in.get('A') is not None and not have_a:
        t = 'uint16' if live_in['A'] == 16 else 'uint8'
        new_params.append((t, 'a'))

    if new_params == params:
        return sig

    params_str = ','.join(f'{t}_{n}' for t, n in new_params) if new_params else ''
    return f'{ret}({params_str})'


def _scan_parent_mx_at(rom: bytes, bank: int, parent_addr: int, parent_end,
                        parent_mo: dict, target_addr: int,
                        exclude_ranges=None,
                        cache: Dict[int, Dict[int, Tuple[int, int]]] = None
                        ) -> Tuple[int, int]:
    """Compute M/X flag state at a sub-entry by decoding the enclosing parent
    function and looking at the call-site M/X of any JSR/JSL that targets
    the sub-entry, or falling back to the nearest prior instruction's M/X.

    Shared between run_config's sub-entry promotion pass and
    promote_sub_entries for sync_funcs_h.
    """
    if cache is None:
        cache = {}
    if parent_addr in cache and target_addr in cache[parent_addr]:
        return cache[parent_addr][target_addr]
    p_end = parent_end if parent_end else target_addr + 0x100
    if p_end > 0xFFFF:
        p_end = 0xFFFF
    try:
        parent_insns = decode_func(rom, bank, parent_addr, end=p_end,
                                   mode_overrides=parent_mo or None,
                                   exclude_ranges=exclude_ranges or None,
                                   validate_branches=False)
    except Exception:
        parent_insns = []
    call_mx = None
    for insn in parent_insns:
        if insn.mnem in ('JSR', 'JSL') and insn.operand == target_addr:
            call_mx = (insn.m_flag, insn.x_flag)
            break
    if call_mx is not None:
        result = call_mx
    else:
        result = (1, 1)
        for insn in sorted(parent_insns, key=lambda i: i.addr):
            local_pc = insn.addr & 0xFFFF
            if local_pc > target_addr:
                break
            result = (insn.m_flag, insn.x_flag)
    cache.setdefault(parent_addr, {})[target_addr] = result
    return result


def promote_sub_entries(rom: bytes, cfg) -> List[Tuple[str, int, str, int]]:
    """Promote `name ADDR NAME sig:...` entries that fall inside an existing
    `func`'s range into first-class func entries. Each sub-entry becomes its
    own function starting at ADDR with the declared sig; the enclosing parent
    is shortened so the split is clean.

    Returns a list of (sub_name, sub_addr, parent_name, parent_addr) tuples
    describing the promotions. Mutates cfg.funcs in place.
    """
    import re as _re
    _existing_func_addrs = {a for _, a, *_ in cfg.funcs}
    _existing_func_names = {n for n, *_ in cfg.funcs}
    _verbatim_defn_re = _re.compile(r'^\s*(?:static\s+)?\w[\w\s\*]*?\s+(\w+)\s*\([^)]*\)\s*\{')
    _verbatim_func_names = set()
    for vline in cfg.verbatim:
        vm = _verbatim_defn_re.match(vline)
        if vm:
            _verbatim_func_names.add(vm.group(1))
    _promoted: List[Tuple[str, int, str, int]] = []
    cache: Dict[int, Dict[int, Tuple[int, int]]] = {}

    for name_full_addr, name_str in list(cfg.names.items()):
        if (name_full_addr >> 16) != cfg.bank:
            continue
        local_addr = name_full_addr & 0xFFFF
        if local_addr in _existing_func_addrs:
            continue
        if name_str in _existing_func_names:
            continue
        if name_str in _verbatim_func_names:
            continue
        parent = None
        for fname, faddr, fsig, fend, fmo, fhints in cfg.funcs:
            if faddr < local_addr:
                if parent is None or faddr > parent[1]:
                    parent = (fname, faddr, fsig, fend, fmo, fhints)
        if parent is None:
            continue
        sub_m, sub_x = _scan_parent_mx_at(
            rom, cfg.bank, parent[1], parent[3], parent[4], local_addr,
            exclude_ranges=cfg.exclude_ranges, cache=cache)
        sub_mode_ovr = {}
        if sub_m == 0 or sub_x == 0:
            flags = 0
            if sub_m == 0: flags |= 0x20
            if sub_x == 0: flags |= 0x10
            sub_mode_ovr[local_addr] = flags
        sub_sig = cfg.sigs.get(name_full_addr)
        cfg.funcs.append((name_str, local_addr, sub_sig, None, sub_mode_ovr, {}))
        _existing_func_addrs.add(local_addr)
        _promoted.append((name_str, local_addr, parent[0], parent[1]))
    cfg.funcs.sort(key=lambda t: t[1])
    return _promoted


def auto_promote_branch_targets(rom: bytes, cfg) -> int:
    """Auto-promote unresolved intra-bank branch targets to sub-entries.

    When a function in bank B branches (BRA/BRL/BEQ/BNE/BCC/BCS/BVS/BVC/JMP)
    to an address A that is inside some OTHER known function's range but is
    NOT that function's entry point, the recompiler currently emits a bare
    `return;` because no callable symbol exists for A. The real ROM branches
    into the middle of code that runs specific setup/cleanup — emitting
    `return` silently skips it.

    This pass scans every function's body for such targets and registers an
    `auto_BB_AAAA` name + sig at each one, so `promote_sub_entries` can
    split the enclosing parent and emit a proper tail call.

    Runs BEFORE promote_sub_entries so the added names get picked up in
    the promotion pass. Returns the number of targets promoted.
    """
    import re as _re
    # Build end-addr map so we know each func's range.
    srt = sorted(cfg.funcs, key=lambda t: t[1])
    ends: Dict[int, int] = {}
    for i, tup in enumerate(srt):
        _, saddr, _, eovr, _, _ = tup
        if eovr is not None:
            ends[saddr] = eovr
        elif i + 1 < len(srt):
            ends[saddr] = srt[i + 1][1]
        else:
            ends[saddr] = 0x10000
    func_entry_addrs = {a for _, a, *_ in cfg.funcs}

    new_targets: Set[int] = set()
    for fname, saddr, _sig, eovr, mo, _h in cfg.funcs:
        if fname in cfg.skip:
            continue
        try:
            insns = decode_func(rom, cfg.bank, saddr, end=ends.get(saddr, 0),
                                mode_overrides=mo or None,
                                exclude_ranges=cfg.exclude_ranges or None,
                                validate_branches=False)
        except Exception:
            continue
        decoded_addrs = {i.addr & 0xFFFF for i in insns}
        for insn in insns:
            mn = insn.mnem
            if mn in ('BPL','BMI','BEQ','BNE','BCC','BCS','BVS','BVC','BRA','BRL'):
                tgt = insn.operand
            elif mn == 'JMP' and insn.mode == ABS:
                tgt = insn.operand
            else:
                continue
            if tgt in decoded_addrs:
                continue  # intra-func branch; already has a label
            if tgt in func_entry_addrs:
                continue  # landing on another func's entry — tail call works
            # Is the target inside SOME known func's range (including this
            # function's own range)? A forward branch whose target isn't in
            # decoded_addrs usually means decode_func gave up following
            # that path — which happens when the target is past the end
            # of decode or the intervening bytes look like data. Promote
            # it to a sub-entry so the decoder can reach it standalone
            # and the emitter can tail-call to it.
            for other_saddr, other_end in ends.items():
                if other_saddr < tgt < other_end:
                    new_targets.add(tgt)
                    break

    # Register each as `auto_BB_AAAA` name + sig:void(). Sub-entry promotion
    # will pick them up; live-in inference (after promotion) augments the
    # sig to match the register conventions at that entry point.
    added = 0
    for tgt in sorted(new_targets):
        full_addr = (cfg.bank << 16) | tgt
        if full_addr in cfg.names:
            continue
        # Skip targets within the last few bytes of the bank: any
        # resulting decode would run off the end and produce garbled
        # code. These usually come from BRA/BRL whose operand-byte
        # interpretation landed on a data byte.
        if tgt >= 0xFFFE:
            continue
        auto_name = f'auto_{cfg.bank:02X}_{tgt:04X}'
        cfg.names[full_addr] = auto_name
        cfg.sigs[full_addr] = 'void()'
        added += 1
    return added


def _sig_matches_dispatch_shape(sig: Optional[str]) -> bool:
    """True if `sig` has a parameter list compatible with a dispatch cast
    (`()` or `(uint8 k)`).

    The return type is irrelevant for dispatch safety: casting a
    RetY/RetAY/uint8-returning function to `FuncU8*` and calling it
    through the pointer just discards the return value at the ABI level
    (the struct is returned in the normal register, the caller ignores
    it). What WOULD crash is a mismatched param count — `void(uint8_k,
    uint8_a)` declared but called with only `k` leaves `a` as
    uninitialised register state in the callee. So we gate only on
    params.

    Consumers that legitimately need the return (direct JSR callers,
    not dispatch callers) still see the full RetAY sig and consume the
    struct normally.
    """
    if sig is None:
        return True
    _ret, params = parse_sig(sig)
    if len(params) == 0:
        return True
    if len(params) == 1 and params[0][1] == 'k':
        return True
    return False


def augment_cfg_sigs_from_livein(rom: bytes, cfg) -> int:
    """Run live-in register inference on every non-skipped function in `cfg`
    and merge the resulting parameters back into `cfg.sigs`.

    Iterates to a fixpoint: a caller's live-in set depends on its callees'
    sigs (JSR to a uint8_k callee counts as reading X in the caller). So
    a change to a callee's sig can propagate a change to its caller on
    the next round. Bounded by a generous iteration cap.

    After live-in fixpoint, a second pass promotes void-returning callees
    that (a) clobber Y and (b) have at least one caller that reads Y
    before writing it after the JSR site to `RetY(...)`. This is the
    bidirectional half of the analysis: consumer-driven rather than
    heuristic. Avoids spuriously demoting functions that merely use Y
    as a scratch register.

    Returns the total number of sig augmentations across all passes. Used
    both by recomp.py's main codegen loop and by sync_funcs_h.py so that
    on-disk funcs.h agrees with what the recompiler emits.
    """
    # Collect dispatch-table targets FIRST so the augment pass knows
    # which functions must keep a FuncU8-compatible sig. Every entry in
    # a jump table gets cast to `FuncU8*` / `FuncV*` at emit time; that
    # cast assumes a fixed 0- or 1-argument shape, so widening a
    # dispatch-target sig leaves later params as garbage at call time.
    cfg.dispatch_target_addrs = _collect_dispatch_targets(rom, cfg)

    total_augmented = 0
    # Always run at least two passes: the first populates cfg.clobbers;
    # the second re-runs live-in with that clobber info in hand, which
    # lets the analysis see through preserve-register helpers. Stopping
    # after pass 1 with `changes == 0` would leave that second-order
    # propagation unrun, and a JSR through a preserve-X helper would
    # look like a destructive call to the caller's live-in pass.
    for _iter in range(8):
        changes = _augment_cfg_sigs_one_pass(rom, cfg)
        total_augmented += changes
        if changes == 0 and _iter >= 1:
            break
    total_augmented += _promote_rety_from_caller_usage(rom, cfg)
    return total_augmented


def _insns_read_reg_post_jsr(caller_insns: List[Insn], jsr_pc: int,
                             target: int, reg: str) -> bool:
    """Walk forward from a JSR/JSL at `jsr_pc` inside `caller_insns` and
    ask whether `reg` is read before it is written (i.e. whether the
    caller consumes the callee's register output). Scans along the
    fall-through path only, stopping at the next transfer instruction.
    """
    insn_by_addr = {i.addr & 0xFFFF: i for i in caller_insns}
    sorted_addrs = sorted(insn_by_addr)
    addr_to_idx = {a: i for i, a in enumerate(sorted_addrs)}
    idx = addr_to_idx.get(jsr_pc & 0xFFFF)
    if idx is None:
        return False
    defined = False
    for j in range(idx + 1, len(sorted_addrs)):
        insn = insn_by_addr[sorted_addrs[j]]
        r, w = _insn_reg_use(insn, reg)
        if r and not defined:
            return True
        if w:
            defined = True
        # Stop following on any transfer; analysing fall-through is
        # enough to catch the common "JSR foo ; TYA ; STA $xxxx" idiom.
        if insn.mnem in ('RTS', 'RTL', 'RTI', 'JMP', 'BRA', 'BRL'):
            break
    return False


def _promote_rety_from_caller_usage(rom: bytes, cfg) -> int:
    """Promote a function's sig to carry Y in its return when intra-bank
    callers consume Y after the JSR site.

      * `void(...)` + Y-clobber + Y-consumer → `RetY(...)`.
      * `uint8(...)` + Y-clobber + Y-consumer → `RetAY(...)` (the function
        already returns A via its declared uint8; promoting to RetAY
        preserves that and adds the Y return on top).

    Returns the number of promotions.
    """
    clobbers = getattr(cfg, 'clobbers', None)
    if not clobbers:
        return 0
    non_skip = [(f, a, s, e, mo, h) for f, a, s, e, mo, h in cfg.funcs
                if f not in cfg.skip]
    non_skip.sort(key=lambda t: t[1])
    # Candidate callees: in this bank, ret is void or uint8, and Y is clobbered.
    candidates: Set[int] = set()
    for fname, addr, *_ in non_skip:
        full = (cfg.bank << 16) | addr
        sig = cfg.sigs.get(full)
        if sig is None:
            sig = 'void()'
        ret, _ps = parse_sig(sig)
        if ret not in ('void', 'uint8'):
            continue
        if 'Y' not in clobbers.get(full, set()):
            continue
        candidates.add(addr)
    if not candidates:
        return 0
    # Decode every caller and look for consumer JSR to a candidate.
    y_consumers: Set[int] = set()
    known_func_addrs: Set[int] = set(cfg.names.keys())
    for _fname, addr, *_ in cfg.funcs:
        known_func_addrs.add((cfg.bank << 16) | addr)
    for i, (fname, start_addr, _sig, eovr, mo, _h) in enumerate(non_skip):
        if eovr is not None:
            end_addr = eovr
        elif i + 1 < len(non_skip):
            end_addr = non_skip[i + 1][1]
        else:
            end_addr = 0x10000
        try:
            insns = decode_func(rom, cfg.bank, start_addr, end=end_addr,
                                mode_overrides=mo or None,
                                exclude_ranges=cfg.exclude_ranges or None,
                                known_func_starts=known_func_addrs,
                                validate_branches=False)
        except Exception:
            continue
        for insn in insns:
            if insn.mnem != 'JSR':
                continue
            tgt = insn.operand & 0xFFFF
            if tgt not in candidates:
                continue
            if _insns_read_reg_post_jsr(insns, insn.addr & 0xFFFF, tgt, 'Y'):
                y_consumers.add(tgt)
    # Promote each consumer. Dispatch-table entries are eligible too:
    # direct-JSR callers still consume the Y return, and dispatch-call
    # sites (through FuncU8 cast) harmlessly discard it at the ABI
    # level. Skipping dispatch targets here regresses direct callers
    # (real case: Spr04C_ExplodingBlock_Init's direct JSR caller writes
    # g_ram[0xc2+k] from the Y return, and skipping promotion degraded
    # it to `= 0 /* UNKNOWN */`).
    promoted = 0
    for tgt in y_consumers:
        full = (cfg.bank << 16) | tgt
        sig = cfg.sigs.get(full, 'void()')
        ret, _ps = parse_sig(sig)
        if ret == 'void':
            params_part = sig[len('void'):] if sig.startswith('void') else '()'
            cfg.sigs[full] = 'RetY' + params_part
            promoted += 1
        elif ret == 'uint8':
            params_part = sig[len('uint8'):] if sig.startswith('uint8') else '()'
            cfg.sigs[full] = 'RetAY' + params_part
            promoted += 1
    return promoted


def _collect_dispatch_targets(rom: bytes, cfg) -> Set[int]:
    """Decode every function once and collect every dispatch-table entry.
    Returns the set of full_addr values that appear as handlers in a
    dispatch table.

    Dispatch tables emit as `FuncU8*` / `FuncV*` casts in the generated
    C. That cast assumes a fixed shape (`void(uint8 k)` or `void()`)
    for every handler, so the augment pass MUST NOT widen any
    dispatch-target function's sig beyond that shape — doing so would
    make the C call pass fewer args than the function declares, leaving
    later parameters as uninitialised stack/register reads. Real crash
    mode: SprStatus01_Init's dispatch of SprXXX_Generic_Init_
    StandardSpritesInit segfaulted when the augment pass added a second
    `uint8 a` param the dispatch cast couldn't pass through.
    """
    non_skip = [(f, a, s, e, mo, h) for f, a, s, e, mo, h in cfg.funcs
                if f not in cfg.skip]
    non_skip.sort(key=lambda t: t[1])
    ends: Dict[int, int] = {}
    for i, tup in enumerate(non_skip):
        _, saddr, _, eovr, _, _ = tup
        if eovr is not None:
            ends[saddr] = eovr
        elif i + 1 < len(non_skip):
            ends[saddr] = non_skip[i + 1][1]
        else:
            ends[saddr] = 0x10000
    known_func_addrs: Set[int] = set(cfg.names.keys())
    for _fname, addr, *_ in cfg.funcs:
        known_func_addrs.add((cfg.bank << 16) | addr)
    targets: Set[int] = set()
    for fname, saddr, _sig, _eovr, mo, _h in non_skip:
        try:
            insns = decode_func(rom, cfg.bank, saddr, end=ends.get(saddr, 0),
                                jsl_dispatch=cfg.jsl_dispatch or None,
                                jsl_dispatch_long=cfg.jsl_dispatch_long or None,
                                mode_overrides=mo or None,
                                dispatch_known_addrs=known_func_addrs,
                                exclude_ranges=cfg.exclude_ranges or None,
                                known_func_starts=known_func_addrs,
                                validate_branches=False)
        except Exception:
            continue
        for insn in insns:
            if insn.dispatch_entries:
                for entry in insn.dispatch_entries:
                    if entry == 0:
                        continue
                    # Dispatch entries are bank-local addresses in almost
                    # all SMW cases; cross-bank dispatch uses long table
                    # entries (`jsl_dispatch_long`) whose emit path is
                    # separate. Store as full_addr so augment can look
                    # up by cfg.sigs key directly.
                    targets.add((cfg.bank << 16) | (entry & 0xFFFF))
    return targets


def _augment_cfg_sigs_one_pass(rom: bytes, cfg) -> int:
    """Single pass of live-in augmentation over every cfg.funcs entry."""
    non_skip = [(f, a, s, e, mo, h) for f, a, s, e, mo, h in cfg.funcs
                if f not in cfg.skip]
    non_skip.sort(key=lambda t: t[1])
    known_func_addrs: Set[int] = set(cfg.names.keys())
    for _fname, addr, *_ in cfg.funcs:
        known_func_addrs.add((cfg.bank << 16) | addr)

    augmented = 0
    for i, (fname, start_addr, sig_tup, eovr, mo, _hints) in enumerate(non_skip):
        if eovr is not None:
            end_addr = eovr
        elif i + 1 < len(non_skip):
            end_addr = non_skip[i + 1][1]
        else:
            end_addr = 0x10000
        full_addr = (cfg.bank << 16) | start_addr
        current_sig = cfg.sigs.get(full_addr, sig_tup)
        try:
            insns = decode_func(
                rom, cfg.bank, start_addr, end=end_addr,
                jsl_dispatch=cfg.jsl_dispatch or None,
                jsl_dispatch_long=cfg.jsl_dispatch_long or None,
                mode_overrides=mo or None,
                exclude_ranges=cfg.exclude_ranges or None,
                known_func_starts=known_func_addrs,
                validate_branches=False)
        except Exception:
            continue
        if not insns:
            continue
        live_in = infer_live_in_regs(insns, start_addr, bank=cfg.bank,
                                     callee_sigs=cfg.sigs,
                                     callee_clobbers=getattr(cfg, 'clobbers', None))
        # Fall-through to next function is a tail call. If the last decoded
        # instruction doesn't transfer control, the function falls through
        # into the next function in ROM order — whose sig's register
        # params become live-in at the fall-through site, and therefore
        # (if no earlier def exists) at entry too. Without this, a tiny
        # stub like `STZ $05 ; REP #$10 ; <falls-through>` loses the X
        # param its fall-through target needs.
        last = insns[-1] if insns else None
        is_terminal = last is not None and (
            last.mnem in ('RTS', 'RTL', 'RTI', 'JMP', 'BRA', 'BRL', 'BRK', 'STP'))
        if not is_terminal and i + 1 < len(non_skip):
            nf_full = (cfg.bank << 16) | non_skip[i + 1][1]
            nf_sig = cfg.sigs.get(nf_full)
            if nf_sig:
                _nr, nf_params = parse_sig(nf_sig)
                pnames = {pn for _pt, pn in nf_params}
                # Only propagate if no local def of that reg exists
                # anywhere in the body (conservative: if the body writes
                # the reg, the fall-through's consumption uses the local
                # def, so it's not live-in).
                for reg, pname in (('A', 'a'), ('X', 'k'), ('Y', 'j')):
                    if live_in.get(reg) is not None:
                        continue
                    if pname not in pnames:
                        continue
                    body_writes = any(
                        _insn_reg_use(ins, reg)[1] for ins in insns
                    )
                    if body_writes:
                        continue
                    live_in[reg] = 8
        new_sig = _augment_sig_with_livein(current_sig, live_in)
        # Record whether this function clobbers A/X/Y without PHA/PLA-style
        # save-restore, so the call-site emitter can drop the caller's
        # register tracking instead of pretending the register was
        # preserved. This is independent of the sig: the sig still
        # describes the C-level return convention, whereas `clobbers`
        # describes which 65816 registers leak across the call boundary.
        if not hasattr(cfg, 'clobbers'):
            cfg.clobbers = {}
        cfg.clobbers[full_addr] = {
            reg for reg in ('A', 'X', 'Y')
            if _writes_register_without_save_restore(insns, reg)
        }
        # Carry-return inference: CLC/SEC + RTS (or join of such exits)
        # without any A writer. SMW uses this idiom for bool-via-carry
        # helpers whose cfg was declared `uint8()` by AUTO but which
        # never actually set A. With carry_ret, the emitter returns the
        # carry expression at RTS instead of the default "A unknown
        # -> 0" fallback.
        if not hasattr(cfg, 'carry_ret'):
            cfg.carry_ret = set()
        if _looks_like_carry_return(insns):
            cfg.carry_ret.add(full_addr)
        # Auto-detect the "LDX $xxxx ; RTS" explicit-restore pattern so
        # callers see X as preserved through the call. Without this, the
        # clobber bit above (X is written internally) makes the call
        # look destructive, but the ROM actually restores X from WRAM
        # before returning. Only populates when the cfg hasn't already
        # declared a restore (explicit cfg still wins).
        if full_addr not in cfg.x_restores:
            x_restore = _detect_x_restore_expr(insns)
            if x_restore:
                cfg.x_restores[full_addr] = x_restore
                # The callee restores X from WRAM before RTS, so from
                # the caller's perspective X is preserved. Remove it
                # from the clobber set so live-in analysis can see
                # through this call on subsequent passes.
                cfg.clobbers[full_addr].discard('X')
        # Dispatch-target guard: if this function is in any dispatch
        # table, the cast site calls it as `FuncU8*` — the param list
        # must stay `()` or `(uint8 k)`. Any wider param list would
        # leave later params as garbage at dispatch-call time. The
        # return type is unrestricted — a RetAY function called via
        # FuncU8 just discards the return at the ABI level, which is
        # the SNES-level behavior anyway.
        #
        # Narrow (not just block widening): if `current_sig` / `new_sig`
        # has excess params from earlier pollution (funcs.h seeded from
        # a prior regen that widened the target), strip them back to
        # a dispatch-compatible param list while preserving the return.
        dispatch_targets = getattr(cfg, 'dispatch_target_addrs', None)
        if (dispatch_targets is not None and full_addr in dispatch_targets
                and new_sig is not None
                and not _sig_matches_dispatch_shape(new_sig)):
            ret_keep, _ps = parse_sig(new_sig)
            new_sig = f'{ret_keep}(uint8_k)'
        if new_sig is not None and new_sig != current_sig:
            cfg.sigs[full_addr] = new_sig
            augmented += 1
    return augmented


def parse_sig(sig: Optional[str]):
    """Parse sig string like 'void(uint8_k)' -> (ret_type, [(type, name), ...])."""
    if sig is None:
        return 'void', [('uint8', 'k')]
    m = re.match(r'(\w[\w*]*)\(([^)]*)\)', sig)
    if not m:
        return 'void', [('uint8', 'k')]
    ret = m.group(1)
    praw = m.group(2).strip()
    if not praw or praw == 'void':
        return ret, []
    params = []
    for tok in praw.split(','):
        tok = tok.strip()
        parts = tok.split('_', 1)
        if len(parts) == 2:
            params.append((parts[0], parts[1]))
        else:
            params.append(('uint8', tok))
    return ret, params


def _fmt_param(t: str, n: str) -> str:
    """Format a parsed (type, name) pair into a C parameter declaration."""
    if n.startswith('*'):
        if t in _STRUCT_PTR_DP_BASE:
            return f'{t} {n}'
        return f'const uint8 {n}'
    return f'{t} {n}'


def format_param_str(params) -> str:
    """Format a list of (type, name) pairs into a C parameter string."""
    return ', '.join(_fmt_param(t, n) for t, n in params)


def _param_to_dp(name: str) -> Optional[int]:
    """Map parameter name to direct page address: r0->$00, r10->$10, R2_W->$02, etc.
    Numbers in param names are HEX (matching 65816 DP convention).
    Also handles compound names like xpos_r10w -> DP $10."""
    clean = name.lstrip('*')
    # r8, r10, rA -> DP $08, $10, $0A
    m = re.match(r'^r([0-9a-fA-F]+)$', clean)
    if m: return int(m.group(1), 16)
    # R0_W, R2_W_ -> DP $00, $02
    m = re.match(r'^R([0-9a-fA-F]+)_W_?$', clean)
    if m: return int(m.group(1), 16)
    # r10w -> DP $10
    m = re.match(r'^r([0-9a-fA-F]+)w$', clean)
    if m: return int(m.group(1), 16)
    # r8_slope_type, r15_foo -> DP $08, $15 (rHEX followed by _descriptor)
    m = re.match(r'^r([0-9a-fA-F]+)_\w+$', clean)
    if m: return int(m.group(1), 16)
    # R0, R2, R15 -> DP $00, $02, $15 (uppercase R + hex, no _W suffix)
    m = re.match(r'^R([0-9a-fA-F]+)$', clean)
    if m: return int(m.group(1), 16)
    # temp14b4, temp14b6 -> WRAM $14B4, $14B6 (temp + hex WRAM address)
    m = re.match(r'^temp([0-9a-fA-F]{3,4})$', clean)
    if m: return int(m.group(1), 16)
    # pN pointer params: p0 -> DP $00, p2 -> DP $02 (long pointer at that DP addr)
    m = re.match(r'^p([0-9a-fA-F]+)$', clean)
    if m: return int(m.group(1), 16)
    # Standalone 'p' -> DP $00 (common indirect long pointer base)
    if clean == 'p': return 0x00
    # Compound names: xpos_r10w, ypos_r12w, etc.
    m = re.search(r'_r([0-9a-fA-F]+)w?$', clean)
    if m: return int(m.group(1), 16)
    m = re.search(r'_R([0-9a-fA-F]+)_W_?$', clean)
    if m: return int(m.group(1), 16)
    return None

# ==============================================================================
# C EMITTER
# ==============================================================================

# DP pointer addresses → C variable name (REMOVED: oracle dependency).
# The emitter now always uses IndirPtr/IndirWriteByte through g_ram,
# which reads the 3-byte long pointer from DP bytes directly.
# This eliminates the need for dp_sync between C pointers and g_ram.
_DP_PTR_MAP: Dict[int, str] = {
    # Intentionally empty — all [$dp],Y accesses go through g_ram.
}

# Struct pointer output params -> DP addresses they write to.
# When a callee has a pointer param of these types, the callee writes
# to these DP addresses. After the call, inject output vars into dp_state.
# Format: {type_name: [(dp_addr_lo, dp_addr_hi, field_name, c_type), ...]}
_STRUCT_OUTPUT_DP: Dict[str, list] = {
    'PointU16': [
        (0x10, 0x11, 'x', 'uint16'),  # pt_out->x at DP $10-$11
        (0x12, 0x13, 'y', 'uint16'),  # pt_out->y at DP $12-$13
    ],
}

# Struct return types -> DP field layout for functions that return structs.
# The callee writes these DP addresses and RTS; we construct the return value
# from dp_state (or g_ram fallback) at the RTS site.
# At call sites, we inject the struct fields back into dp_state so subsequent
# DP reads see the correct values.
# Format: {type_name: [(dp_addr, field_name, ctype), ...]}
_STRUCT_RETURN_DP: Dict[str, list] = {
    'PointU8': [
        (0x00, 'x', 'uint8'),   # x at DP $00
        (0x01, 'y', 'uint8'),   # y at DP $01
    ],
    'PointU16': [
        (0x00, 'x', 'uint16'),  # x at DP $00 (uint16)
        (0x02, 'y', 'uint16'),  # y at DP $02 (uint16)
    ],
    'OwHvPos': [
        (0x00, 'r0', 'uint16'), # r0 at DP $00 (uint16)
        (0x02, 'r2', 'uint16'), # r2 at DP $02 (uint16)
        (0x06, 'r6', 'uint16'), # r6 at DP $06 (uint16)
        (0x08, 'r8', 'uint16'), # r8 at DP $08 (uint16)
    ],
    # PairU16 returns via A/X registers, handled specially in _emit_call.
    'HdmaPtrs': [
        (0x04, 'r4', 'u8ptr'),  # const uint8 * r4 at DP $04 (WRAM offset)
        (0x06, 'r6', 'u8ptr'),  # const uint8 * r6 at DP $06
    ],
    'PairU8': [
        (0x02, 'first', 'uint8'),   # first (r2) at DP $02
        (0x03, 'second', 'uint8'),  # second (r3) at DP $03
    ],
}

# Struct pointer params -> DP base address.
# These struct types overlay g_ram at the given DP offset.
# When a function takes a struct pointer, pass (StructType*)(g_ram + base).
_STRUCT_PTR_DP_BASE: Dict[str, int] = {
    'CollInfo': 0x00,       # CollInfo fields r0-r11 live at DP $00-$0B
    'ExtCollOut': 0x0A,     # ExtCollOut fields r10-r13 live at DP $0A-$0D
    'PointU16': 0x10,       # PointU16 fields x,y at DP $10-$13
    'HdmaPtrs': 0x04,       # HdmaPtrs r4,r6 at DP $04,$06
    'CalcTiltPlatformArgs': 0x14B0,  # Fields at WRAM $14B0,$14B2,$14BC,$14BF
    'CheckPlatformCollRet': 0x10,    # Contains PointU16 at $10 + bool fields
    'PointU8': 0x00,                 # PointU8 fields x,y at DP $00-$01
    'PairU8': 0x00,                  # PairU8 fields first,second at DP $00-$03
}

# ---------------------------------------------------------------------------
# Optional inline symbol comments (loaded from --symbols JSON)
# ---------------------------------------------------------------------------
_ram_symbols: Dict[int, str] = {}   # WRAM addr -> label name
_reg_symbols: Dict[int, str] = {}   # HW register addr -> label name


def load_symbols(path: str):
    """Load symbol JSON (from parse_smwdisx_symbols.py) into module globals."""
    global _ram_symbols, _reg_symbols
    with open(path) as f:
        data = json.load(f)
    # Convert hex-string keys to int keys for fast lookup
    _ram_symbols = {int(k, 16): v for k, v in data.get('ram', {}).items()}
    _reg_symbols = {int(k, 16): v for k, v in data.get('reg', {}).items()}


class EmitCtx:
    """Tracks abstract register state and emits C statements.

    All variable declarations are hoisted to the function top with zero-init
    to prevent UB when gotos skip initialization (HANDOFF requirement D).
    """

    def __init__(self, bank: int, func_names: Dict[int, str],
                 func_sigs: Dict[int, str] = None,
                 init_x: Optional[str] = 'k', init_a: Optional[str] = None,
                 init_b: Optional[str] = None,
                 init_carry: Optional[str] = None,
                 ret_type: str = 'void', func_start: int = 0,
                 valid_branch_targets: Set[int] = None,
                 backward_branch_targets: Set[int] = None,
                 dp_sync: Dict[int, str] = None,
                 rom: bytes = None,
                 carry_ret: bool = False,
                 x_restores_map: Dict[int, str] = None,
                 y_after_map: Dict[int, int] = None,
                 x_after_map: Dict[int, int] = None,
                 callee_clobbers: Dict[int, Set[str]] = None):
        self.bank = bank
        self.func_names = func_names
        self.func_sigs = func_sigs or {}
        self.ret_type = ret_type
        self.func_start = func_start & 0xFFFF
        self.valid_branch_targets = valid_branch_targets or set()
        self._backward_branch_targets = backward_branch_targets or set()
        self._rom_bytes = rom
        self._carry_ret = carry_ret  # return carry flag at RTS instead of A
        self._ret_y = False  # return Y register instead of A at RTS
        self.x_restores_map: Dict[int, str] = x_restores_map or {}  # callee -> X expr
        self.y_after_map: Dict[int, int] = y_after_map or {}  # callee -> Y increment
        self.x_after_map: Dict[int, int] = x_after_map or {}  # callee -> X increment
        # Per-callee set of registers the callee clobbers (writes without a
        # matching PH/PL save-restore pair). Used at JSR/JSL sites to drop
        # the caller's tracking of any clobbered register, so we don't
        # pretend the register was preserved and emit stale values.
        self.callee_clobbers: Dict[int, Set[str]] = callee_clobbers or {}
        self.end_addr: int = 0  # function end address (for cross-function branch detection)

        # Whether the previous instruction was an unconditional transfer
        # (BRA/BRL/JMP/RTS/RTL/RTI/tail-call). If True, the current register
        # state is dead — no path from the previous PC falls through to the
        # next instruction, so label-target merges must not use self.A/X/Y
        # as "the fall-through value".
        self._prev_terminal: bool = False

        # Abstract register values (C expression strings, or None=unknown)
        self.A: Optional[str] = init_a
        self.B: Optional[str] = init_b  # 65816 B accumulator (high byte, swapped via XBA)
        self.X: Optional[str] = init_x
        self._init_x: Optional[str] = init_x  # saved for PLX heuristic
        self._last_pha_val: Optional[str] = None  # saved for branch-forked PLA
        self._stk_vars: Set[str] = set()  # stack-relative variable names
        self.has_k = (init_x is not None)
        self.Y: Optional[str] = None
        self.stack: List[Tuple[str, Optional[str]]] = []

        # Carry tracking
        self.carry: Optional[str] = init_carry  # '0', '1', expr, or None
        self.carry_chain: Optional[dict] = None # {'var': name, 'expr': str}

        # Flag source for branches
        self.flag_src: Optional[str] = None
        self.flag_width: int = 8  # 8 or 16: width of last flag-setting operation
        self.overflow: Optional[str] = None  # V flag expression (bit 6 from BIT)

        # Branch merge tracking: when a conditional branch is taken, save the
        # register state at the branch source. At the branch target label, if
        # the fall-through state differs, emit a conditional (phi node).
        # Maps target_pc -> {'A': expr, 'X': expr, 'Y': expr, 'carry': expr, 'cond': expr}
        self._branch_states: Dict[int, dict] = {}

        # DP write tracking for parameter passing (HANDOFF requirement E)
        self.dp_state: Dict[int, str] = {}

        # ORACLE BRIDGE: dp_sync --{dp_addr: sync_func_name}
        # Call sync function after writing to these DP addresses.
        # Remove when all banks are recompiled and oracle is fully replaced.
        self.dp_sync: Dict[int, str] = dp_sync or {}

        # Output and variable management
        self.lines: List[str] = []
        # Hoisted declarations: {var_name: (type, initial_value_expr_or_None)}
        self._hoisted: Dict[str, str] = {}  # name -> type
        self._var_n = 1
        self._tmp_n = 1
        self._cur_a_type = 'uint8'
        self._cur_x_type = 'uint8'

    # -- Variable allocation --------------------------------------------------

    def _alloc(self, type_: str = 'uint8') -> str:
        """Allocate a hoisted variable. Returns the variable name."""
        name = f'v{self._var_n}'
        self._var_n += 1
        self._hoisted[name] = type_
        return name

    def _alloc_tmp(self, type_: str = 'uint16') -> str:
        """Allocate a hoisted temp variable for carry chains etc."""
        name = f'tmp{self._tmp_n}'
        self._tmp_n += 1
        self._hoisted[name] = type_
        return name

    def _return_value_expr(self) -> Optional[str]:
        """Return the C expression for `return <expr>` matching the current
        function's ret_type, or None if the function is void.

        Handles every non-void return convention the recompiler understands:
        struct returns via DP writes, carry-flag returns (bool-via-carry),
        uint16 (A/X combined), PairU16 / RetAY multi-register structs, and
        RetY / ret-A scalar returns. Used by both the RTS handler and the
        early-exit emitters (branch-as-return, JMP-as-return) so all exit
        paths of a function agree on the return convention.
        """
        rt = self.ret_type
        if rt == 'void':
            return None
        if rt in _STRUCT_RETURN_DP:
            return self._struct_ret_expr(rt)
        if self._carry_ret and self.carry is not None:
            return f'({self.carry}) ? 1 : 0'
        if rt == 'uint16':
            if self.X is not None:
                return self.X
            if self.A is not None:
                return self.A
            self._warn('X and A unknown at uint16 return --returning 0')
            return '0'
        if rt == 'PairU16':
            a_val = self.A if self.A is not None else '0'
            x_val = self.X if self.X is not None else '0'
            return f'(PairU16){{ .first = {a_val}, .second = {x_val} }}'
        if rt == 'RetAY':
            a_val = self.A if self.A is not None else '0'
            y_val = self.Y if self.Y is not None else '0'
            return f'(RetAY){{ .a = {a_val}, .y = {y_val} }}'
        if rt == 'RetY':
            y_val = self.Y if self.Y is not None else '0'
            return f'(RetY){{ .y = {y_val} }}'
        if self._ret_y:
            return self.Y if self.Y is not None else '0'
        if self.A is None:
            self._warn('A unknown at return --returning 0')
        return self.A if self.A is not None else '0'

    def _emit_return_for_current_sig(self):
        """Emit the C `return [expr];` line for the current ret_type."""
        expr = self._return_value_expr()
        if expr is None:
            self._emit('return;')
        else:
            self._emit(f'return {expr};')

    def _struct_ret_expr(self, ret_type: str) -> str:
        """Build struct construction expression for a struct return at RTS/RTL.

        For uint8 fields: uses dp_state values where available (set by 8-bit STA).
        For uint16 fields: always reads from g_ram (the wide STA wrote both bytes
        to WRAM; the dp_state variable may have been re-assigned since the write).
        """
        parts = []
        for dp_addr, field, ctype in _STRUCT_RETURN_DP[ret_type]:
            val = self.dp_state.get(dp_addr)
            if ctype == 'u8ptr':
                # const uint8 *: the ROM stores a 16-bit WRAM offset in
                # DP[dp_addr..dp_addr+1]; reconstruct the C pointer as
                # `g_ram + offset`. (Always read from g_ram — dp_state
                # fields are per-byte and don't give us a clean offset.)
                parts.append(
                    f'.{field} = g_ram + PAIR16(g_ram[0x{dp_addr+1:02x}], '
                    f'g_ram[0x{dp_addr:02x}])')
            elif val and ctype != 'uint16':
                parts.append(f'.{field} = {val}')
            elif ctype == 'uint16':
                parts.append(f'.{field} = PAIR16(g_ram[0x{dp_addr+1:02x}], g_ram[0x{dp_addr:02x}])')
            else:
                parts.append(f'.{field} = g_ram[0x{dp_addr:02x}]')
        return f'({ret_type}){{ {", ".join(parts)} }}'

    def _simple(self, expr: str) -> bool:
        """True if expr is a bare C identifier."""
        return bool(re.match(r'^[a-zA-Z_]\w*$', expr)) if expr else False

    def _materialize_refs_to(self, var: str):
        """If any pending register expression references `var`, materialize it
        before `var` is modified (INX/INY/DEX/DEY). Prevents stale-reference
        bugs where LDA sets A to an expression containing Y, then INY changes Y
        before STA materializes A --evaluating with the wrong Y value."""
        for reg, type_ in [('A', self._cur_a_type), ('X', self._cur_x_type),
                           ('Y', self._cur_x_type)]:
            val = getattr(self, reg)
            if val is not None and not self._simple(val) and var in val:
                self._materialize(reg, type_)

    def _materialize(self, reg: str, type_: str = 'uint8') -> str:
        """Ensure register holds a named variable; allocate+assign if not."""
        val = getattr(self, reg)
        if val is None:
            name = self._alloc(type_)
            self._warn(f'{reg} unknown --emitting 0 fallback',
                       f'Trace upstream LDA/LDX/LDY')
            self._emit(f'{name} = 0;')
            setattr(self, reg, name)
            return name
        if self._simple(val):
            return val
        name = self._alloc(type_)
        self._emit(f'{name} = {val};')
        setattr(self, reg, name)
        return name

    def _ensure_mutable_x(self, x_type: str = 'uint8') -> Optional[str]:
        """Ensure X holds a mutable variable (not k/j parameter). If X is a
        function parameter, create a mutable copy. Returns var name or None."""
        xn = self.X
        if xn is None:
            return None
        if xn in ('k', 'j'):
            # Create a mutable copy of the parameter
            name = self._alloc(x_type)
            self._emit(f'{name} = {xn};')
            self.X = name
            return name
        if self._simple(xn):
            return xn
        # Complex expression --materialize
        return self._materialize('X', x_type)

    # -- Output helpers -------------------------------------------------------

    def _emit(self, stmt: str):
        self.lines.append('  ' + stmt)

    def _warn(self, msg: str, fix: str = ''):
        if fix:
            self.lines.append(f'  /* RECOMP_WARN: {msg} --Fix: {fix} */')
        else:
            self.lines.append(f'  /* RECOMP_WARN: {msg} */')

    def _idx(self, reg: str) -> str:
        val = getattr(self, reg)
        return val if val is not None else '0 /* UNKNOWN */'

    # -- Memory expression builders -------------------------------------------

    @staticmethod
    def _sym(addr: int) -> str:
        """Return inline symbol comment for a WRAM address, or empty string."""
        name = _ram_symbols.get(addr)
        return f' /* {name} */' if name else ''

    @staticmethod
    def _reg_sym(addr: int) -> str:
        """Return inline symbol comment for a hardware register, or empty string."""
        name = _reg_symbols.get(addr)
        return f' /* {name} */' if name else ''

    def _wram(self, addr: int, idx: str) -> str:
        sym = self._sym(addr) if idx in ('0', '0x0') else ''
        if idx in ('0', '0x0'): return f'g_ram[0x{addr:x}]{sym}'
        return f'g_ram[0x{addr:x} + {idx}]'

    def _wram16(self, addr: int, idx: str) -> str:
        """16-bit WRAM read: GET_WORD(g_ram + addr + idx)"""
        sym = self._sym(addr) if idx in ('0', '0x0') else ''
        if idx in ('0', '0x0'): return f'GET_WORD(g_ram + 0x{addr:x}){sym}'
        return f'GET_WORD(g_ram + 0x{addr:x} + {idx})'

    def _wram16_write(self, addr: int, idx: str, val: str):
        """16-bit WRAM write."""
        sym = self._sym(addr) if idx in ('0', '0x0') else ''
        if idx in ('0', '0x0'):
            self._emit(f'*(uint16*)(g_ram + 0x{addr:x}) = {val};{sym}')
        else:
            self._emit(f'*(uint16*)(g_ram + 0x{addr:x} + {idx}) = {val};')

    def _rom(self, full_addr: int, idx: str) -> str:
        bk = (full_addr >> 16) & 0xFF
        addr = full_addr & 0xFFFF
        return f'RomPtr_{bk:02X}(0x{addr:x})[{idx}]'

    def _rom_ptr(self, full_addr: int, idx: str) -> str:
        bk = (full_addr >> 16) & 0xFF
        addr = full_addr & 0xFFFF
        if idx in ('0', '0x0'):
            return f'RomPtr_{bk:02X}(0x{addr:x})'
        return f'(RomPtr_{bk:02X}(0x{addr:x}) + {idx})'

    def _rom16(self, full_addr: int, idx: str) -> str:
        """16-bit ROM read."""
        return f'GET_WORD({self._rom_ptr(full_addr, idx)})'

    def _callee(self, full_addr: int) -> str:
        return self.func_names.get(full_addr, f'func_{full_addr:06x}')

    def _wrap(self, expr: str) -> str:
        return expr if self._simple(expr) else f'({expr})'

    # -- Indirect addressing helpers ------------------------------------------

    def _indir_read(self, dp: int, y_expr: str, wide: bool = False) -> str:
        """LDA [dp],Y or LDA (dp),Y --read through pointer at DP address."""
        if dp in _DP_PTR_MAP:
            ptr = _DP_PTR_MAP[dp]
            idx = '0' if y_expr == '0' else y_expr
            if wide:
                return f'GET_WORD({ptr} + {idx})' if idx != '0' else f'GET_WORD({ptr})'
            return f'{ptr}[{idx}]'
        if wide:
            return f'GET_WORD(IndirPtr(*(LongPtr*)(g_ram+0x{dp:x}), {y_expr}))'
        return f'IndirPtr(*(LongPtr*)(g_ram+0x{dp:x}), {y_expr})[0]'

    def _indir_write(self, dp: int, y_expr: str, val: str):
        if dp in _DP_PTR_MAP:
            ptr = _DP_PTR_MAP[dp]
            if y_expr == '0':
                self._emit(f'{ptr}[0] = {val};')
            else:
                self._emit(f'{ptr}[{y_expr}] = {val};')
        else:
            self._emit(f'IndirWriteByte(*(LongPtr*)(g_ram+0x{dp:x}), {y_expr}, {val});')

    def _dp_indir_addr(self, dp: int) -> str:
        """Read 16-bit address from DP: (g_ram[$dp] | g_ram[$dp+1]<<8)"""
        return f'(g_ram[0x{dp:02x}] | (g_ram[0x{dp:02x} + 1] << 8))'

    # -- Hardware register detection --------------------------------------------

    @staticmethod
    def _is_hw_reg(addr: int) -> bool:
        """True if addr is in a SNES hardware register range (absolute addressing)."""
        return (0x2100 <= addr <= 0x21FF or   # PPU / APU ports
                0x4200 <= addr <= 0x43FF)      # CPU / DMA / joypad

    # -- Memory operand resolver (unified for ALU ops) ------------------------

    def _resolve_mem(self, mode: int, v: int, wide: bool = False) -> Optional[str]:
        """Return C expression for memory operand in given mode, or None."""
        if mode == IMM:
            return str(v) if v < 10 else f'0x{v:x}'
        elif mode == DP:
            if not wide:
                dp_val = self.dp_state.get(v)
                if dp_val is not None:
                    return dp_val
            return self._wram16(v, '0') if wide else self._wram(v, '0')
        elif mode == DP_X:
            return self._wram16(v, self._idx('X')) if wide else self._wram(v, self._idx('X'))
        elif mode == DP_Y:
            return self._wram16(v, self._idx('Y')) if wide else self._wram(v, self._idx('Y'))
        elif mode == ABS:
            # LoROM: addresses >= $8000 are ROM, < $8000 are WRAM (mirrors)
            if v >= 0x8000:
                full = (self.bank << 16) | v
                return self._rom16(full, '0') if wide else self._rom(full, '0')
            if self._is_hw_reg(v):
                return f'ReadRegWord(0x{v:x})' if wide else f'ReadReg(0x{v:x})'
            return self._wram16(v, '0') if wide else self._wram(v, '0')
        elif mode == ABS_X:
            if v >= 0x8000:
                full = (self.bank << 16) | v
                return self._rom16(full, self._idx('X')) if wide else self._rom(full, self._idx('X'))
            if self._is_hw_reg(v):
                x = self._idx('X')
                return f'ReadRegWord(0x{v:x} + {x})' if wide else f'ReadReg(0x{v:x} + {x})'
            return self._wram16(v, self._idx('X')) if wide else self._wram(v, self._idx('X'))
        elif mode == ABS_Y:
            if v >= 0x8000:
                full = (self.bank << 16) | v
                return self._rom16(full, self._idx('Y')) if wide else self._rom(full, self._idx('Y'))
            if self._is_hw_reg(v):
                y = self._idx('Y')
                return f'ReadRegWord(0x{v:x} + {y})' if wide else f'ReadReg(0x{v:x} + {y})'
            return self._wram16(v, self._idx('Y')) if wide else self._wram(v, self._idx('Y'))
        elif mode == LONG:
            bk = (v >> 16) & 0xFF
            if bk in (0x7E, 0x7F):
                addr = v & 0xFFFF
                base = addr if bk == 0x7E else (0x10000 + addr)
                return self._wram16(base, '0') if wide else self._wram(base, '0')
            if bk == 0x70:
                addr = v & 0xFFFF
                return f'GET_WORD(g_sram + 0x{addr:x})' if wide else f'g_sram[0x{addr:x}]'
            return self._rom16(v, '0') if wide else self._rom(v, '0')
        elif mode == LONG_X:
            bk = (v >> 16) & 0xFF
            if bk in (0x7E, 0x7F):
                addr = v & 0xFFFF
                base = addr if bk == 0x7E else (0x10000 + addr)
                return self._wram16(base, self._idx('X')) if wide else self._wram(base, self._idx('X'))
            if bk == 0x70:
                addr = v & 0xFFFF
                x = self._idx('X')
                return f'GET_WORD(g_sram + 0x{addr:x} + {x})' if wide else f'g_sram[0x{addr:x} + {x}]'
            return self._rom16(v, self._idx('X')) if wide else self._rom(v, self._idx('X'))
        elif mode == INDIR_LY:
            return self._indir_read(v, self._idx('Y'), wide=wide)
        elif mode == INDIR_L:
            return self._indir_read(v, '0', wide=wide)
        elif mode == INDIR_Y:
            # LDA ($dp),Y — 16-bit indirect (NOT long). Read 2-byte pointer from DP, add Y, access WRAM.
            addr_expr = f'(g_ram[0x{v:02x}] | (g_ram[0x{v:02x} + 1] << 8))'
            y_expr = self._idx('Y')
            if wide:
                return f'GET_WORD(g_ram + {addr_expr} + {y_expr})'
            return f'g_ram[{addr_expr} + {y_expr}]'
        elif mode == INDIR_DPX:
            addr_expr = f'(g_ram[0x{v:02x} + {self._idx("X")}] | (g_ram[0x{v:02x} + {self._idx("X")} + 1] << 8))'
            if wide:
                return f'GET_WORD(g_ram + {addr_expr})'
            return f'g_ram[{addr_expr}]'
        elif mode == DP_INDIR:
            addr_expr = self._dp_indir_addr(v)
            if wide:
                return f'GET_WORD(g_ram + {addr_expr})'
            return f'g_ram[{addr_expr}]'
        elif mode == STK:
            # Stack-relative: treat as named local variable
            vname = f'stk_{v:02x}'
            self._stk_vars.add(vname)
            return vname
        elif mode == STK_IY:
            # Stack-relative indirect indexed: (stk,S),Y
            vname = f'stk_{v:02x}'
            self._stk_vars.add(vname)
            return f'g_ram[(uint16){vname} + {self._idx("Y")}]'
        return None

    # -- Call argument builder ------------------------------------------------

    def _build_call_args(self, params) -> str:
        if not params:
            return ''
        args = []
        for _type, name in params:
            if name == 'k':
                args.append(self._idx('X') if self.X is not None else
                            '0 /* RECOMP_WARN: X unknown at call site */')
            elif name == 'j':
                # 65816 convention: j is typically in Y (secondary index).
                # Use Y if available, fall back to X.
                if self.Y is not None:
                    args.append(self._idx('Y'))
                elif self.X is not None:
                    args.append(self._idx('X'))
                else:
                    args.append('0 /* RECOMP_WARN: j unknown at call site */')
            elif name == 'a':
                if _type == 'uint16' and self.B is not None and self.A is not None:
                    # XBA idiom: caller loaded hi->XBA->lo to set B:A as a 16-bit pair.
                    # If B = HIBYTE(A), the pair reconstructs A --pass it directly.
                    a_simple = self.A if self._simple(self.A) else None
                    if a_simple and (self.B == f'HIBYTE({a_simple})' or self.B == f'(uint8)(({a_simple}) >> 8)'):
                        args.append(a_simple)
                    else:
                        args.append(f'PAIR16({self.B}, {self.A})')
                else:
                    args.append(self.A if self.A is not None else
                                '0 /* RECOMP_WARN: A unknown at call site */')
            else:
                dp_addr = _param_to_dp(name)
                dp_val = self.dp_state.get(dp_addr) if dp_addr is not None else None
                if dp_val is not None:
                    if name.startswith('*'):
                        args.append(f'g_ram + {dp_val}')
                    else:
                        args.append(dp_val)
                elif dp_addr is not None:
                    # DP param not in tracked state --read from g_ram.
                    # This handles values set by callees (e.g. struct output params).
                    bare = name.lstrip('*')
                    if name.startswith('*'):
                        args.append(f'&g_ram[0x{dp_addr:02x}]')
                    elif _type == 'uint16':
                        args.append(f'PAIR16(g_ram[0x{dp_addr+1:02x}], g_ram[0x{dp_addr:02x}])')
                    else:
                        args.append(f'g_ram[0x{dp_addr:02x}]')
                elif name == 'cr':
                    # cr = carry register --pass carry flag state
                    if self.carry is not None:
                        args.append(f'({self.carry}) ? 1 : 0')
                    else:
                        args.append(f'0 /* RECOMP_WARN: param cr (carry) unknown */')
                elif name.startswith('*') and _type in _STRUCT_PTR_DP_BASE:
                    # Struct pointer param --resolve to (StructType*)(g_ram + base)
                    base = _STRUCT_PTR_DP_BASE[_type]
                    args.append(f'({_type}*)(g_ram + 0x{base:02x})')
                elif _type in _STRUCT_PTR_DP_BASE and not name.startswith('*'):
                    # Struct by-value param --first check dp_state for a recently
                    # returned struct variable (fields stored by _STRUCT_RETURN_DP).
                    struct_var = None
                    if _type in _STRUCT_RETURN_DP:
                        fields = _STRUCT_RETURN_DP[_type]
                        candidate = None
                        ok = True
                        for dp_addr, field, _ct in fields:
                            val = self.dp_state.get(dp_addr)
                            if val and '.' in val:
                                var, fld = val.rsplit('.', 1)
                                if fld == field:
                                    if candidate is None:
                                        candidate = var
                                    elif candidate != var:
                                        ok = False; break
                                else:
                                    ok = False; break
                            else:
                                ok = False; break
                        if ok and candidate:
                            struct_var = candidate
                    if struct_var:
                        args.append(struct_var)
                    elif _type in _STRUCT_RETURN_DP:
                        # Fall back to return DP base (where struct-returning callees
                        # write their result, e.g. PointU16 -> DP $00/$02).
                        base = _STRUCT_RETURN_DP[_type][0][0]
                        args.append(f'*({_type}*)(g_ram + 0x{base:02x})')
                    else:
                        base = _STRUCT_PTR_DP_BASE[_type]
                        args.append(f'*({_type}*)(g_ram + 0x{base:02x})')
                else:
                    bare = name.lstrip('*')
                    scalar_types = {'uint8','uint16','int8','int16','int','bool','uint32','int32'}
                    if name.startswith('*'):
                        args.append(f'NULL /* RECOMP_WARN: param {bare} unknown */')
                    elif bare == 'sign' and self.flag_src is not None:
                        # 'sign' param = N flag from last comparison (BMI/BPL)
                        sign_t = 'int16' if self.flag_width == 16 else 'int8'
                        args.append(f'(({sign_t})({self.flag_src}) < 0) ? 1 : 0')
                    elif _type in scalar_types or _type.endswith('*'):
                        # Heuristic: unnamed scalar params may be passed in A
                        if self.A is not None and bare not in ('k', 'j'):
                            args.append(self.A)
                        else:
                            args.append(f'0 /* RECOMP_WARN: param {name} unknown */')
                    else:
                        args.append(f'({_type}){{0}} /* RECOMP_WARN: param {name} unknown */')
        return ', '.join(args)

    # -- Tail call emission ---------------------------------------------------

    def _emit_tail_call(self, v: int, cond: str = None) -> bool:
        """Emit a tail call to a known function. If cond is given, wrap in if().

        When the outer's return type differs from the callee's, bridge
        them with the appropriate wrap/destructure so the C type checks.
        The tail call is semantically `return callee()` but must match
        the outer's declared return struct/scalar.
        """
        full_addr = (self.bank << 16) | v
        if full_addr not in self.func_names:
            return False
        fname = self.func_names[full_addr]
        callee_sig = self.func_sigs.get(full_addr)
        _ret, callee_params = parse_sig(callee_sig)
        call_args = self._build_call_args(callee_params)
        call_expr = f'{fname}({call_args})'
        outer = self.ret_type

        def _bridge_return(callee_ret: str, outer_ret: str, expr: str) -> str:
            """Return a C expression for `return <...>` that satisfies the
            outer's declared return type given an expression of callee_ret
            type. Used to bridge uint8 <-> RetAY / RetY mismatches across
            tail calls."""
            if callee_ret == outer_ret:
                return f'return {expr};'
            if callee_ret == 'void':
                # Discard; rebuild outer's return from tracked state.
                rv = self._return_value_expr()
                rv_suffix = '' if rv is None else f' {rv}'
                return f'{expr}; return{rv_suffix};'
            # Callee has a concrete retval, wrap/unwrap to fit outer.
            if outer_ret == 'void':
                return f'{expr}; return;'
            if callee_ret == outer_ret:
                return f'return {expr};'
            # uint8/uint16 outer, struct callee → pick the .a field.
            if outer_ret in ('uint8', 'uint16') and callee_ret in ('RetAY', 'RetY'):
                field = 'a' if callee_ret == 'RetAY' else 'y'
                tmp = self._alloc(callee_ret)
                self._emit(f'{tmp} = {expr};')
                return f'return {tmp}.{field};'
            # Struct outer, scalar callee → wrap into a struct literal.
            if outer_ret == 'RetY' and callee_ret in ('uint8', 'uint16'):
                return f'return (RetY){{ .y = {expr} }};'
            if outer_ret == 'RetAY' and callee_ret in ('uint8', 'uint16'):
                return f'return (RetAY){{ .a = {expr}, .y = 0 }};'
            # Mixed struct types (RetAY ↔ RetY etc.) — destructure and
            # rebuild by common field name.
            if outer_ret == 'RetAY' and callee_ret == 'RetY':
                tmp = self._alloc(callee_ret)
                self._emit(f'{tmp} = {expr};')
                return f'return (RetAY){{ .a = 0, .y = {tmp}.y }};'
            if outer_ret == 'RetY' and callee_ret == 'RetAY':
                tmp = self._alloc(callee_ret)
                self._emit(f'{tmp} = {expr};')
                return f'return (RetY){{ .y = {tmp}.y }};'
            # Last resort: emit unchanged and let the C compiler flag it.
            return f'return {expr};'

        # Tail calls must pop this frame before returning (see RTL/RTS handler).
        if cond:
            # Conditional tail call: if (cond) { call; return; }
            if _ret != 'void' and outer != 'void':
                ret_stmt = _bridge_return(_ret, outer, call_expr)
                self._emit(f'if ({cond}) {{ RecompStackPop(); {ret_stmt} }}')
            elif _ret != 'void':
                # Callee has a retval we discard; outer is void.
                self._emit(f'if ({cond}) {{ {call_expr}; RecompStackPop(); return; }}')
            elif outer != 'void':
                # Callee is void; outer returns A (or RetAY etc.). After
                # the call A is unknown, so fall back to
                # _return_value_expr's defaulting ('0' when register is
                # untracked).
                rv = self._return_value_expr()
                self._emit(f'if ({cond}) {{ {call_expr}; RecompStackPop(); return {rv}; }}')
            else:
                self._emit(f'if ({cond}) {{ {call_expr}; RecompStackPop(); return; }}')
        else:
            if _ret != 'void' and outer != 'void':
                self._emit('RecompStackPop();')
                self._emit(_bridge_return(_ret, outer, call_expr))
            elif _ret != 'void':
                tmp = self._alloc(_ret)
                self._emit(f'{tmp} = {call_expr};')
                self._emit('RecompStackPop();')
                self._emit('return;')
            else:
                self._emit(f'{call_expr};')
                self._emit('RecompStackPop();')
                self._emit_return_for_current_sig()
        return True

    # -- Branch condition builder ---------------------------------------------

    def _branch_cond(self, mnem: str) -> str:
        fs = self.flag_src
        s = self._wrap(fs) if fs and not self._simple(fs) else (fs or '0 /* flags unknown */')
        if mnem == 'BEQ': return f'{s} == 0'
        if mnem == 'BNE': return f'{s} != 0'
        sign_t = 'int16' if self.flag_width == 16 else 'int8'
        if mnem == 'BPL': return f'({sign_t}){s} >= 0'
        if mnem == 'BMI': return f'({sign_t}){s} < 0'
        if mnem == 'BCS': return f'{self.carry} != 0' if self.carry else '/* carry? */ 0'
        if mnem == 'BCC': return f'{self.carry} == 0' if self.carry else '/* carry? */ 0'
        if mnem == 'BVS': return f'{self.overflow}' if self.overflow else '/* overflow? */ 0'
        if mnem == 'BVC': return f'!({self.overflow})' if self.overflow else '/* !overflow? */ 0'
        return f'/* {mnem} */ 0'

    # ==========================================================================
    # MAIN INSTRUCTION EMITTER
    # ==========================================================================

    def emit(self, insn: Insn, branch_targets: Set[int]):
        mn = insn.mnem
        v = insn.operand
        mode = insn.mode
        pc = insn.addr & 0xFFFF
        wide_a = (insn.m_flag == 0)    # 16-bit accumulator
        wide_x = (insn.x_flag == 0)    # 16-bit index
        a_type = 'uint16' if wide_a else 'uint8'
        x_type = 'uint16' if wide_x else 'uint8'
        self._cur_a_type = a_type
        self._cur_x_type = x_type
        # Auto-reset flag_width when any non-REP/SEP/branch instruction runs.
        # CMP/CPX/CPY in 16-bit mode will override to 16 explicitly.
        # REP/SEP don't set flag_src, so they must not reset flag_width
        # (the 16-bit CMP → SEP → BPL pattern requires flag_width=16
        # to survive across the SEP).
        # Branch instructions consume flag_width, so they must not reset it.
        _NO_FLAG_RESET = ('REP', 'SEP', 'BPL', 'BMI', 'BEQ', 'BNE', 'BCS', 'BCC', 'BVS', 'BVC')
        if mn not in _NO_FLAG_RESET:
            # Set flag_width based on the CURRENT accumulator width for this
            # instruction, so 16-bit EOR/AND/ORA/ADC/SBC/LDA get int16 sign checks.
            self.flag_width = 16 if wide_a else 8

        # Emit label with branch-merge
        if pc in branch_targets:
            # Loop header pre-materialization: if this label is a backward branch
            # target (loop header) and X/Y are frozen parameters, create mutable
            # copies BEFORE the label so loop iterations accumulate correctly.
            # The assignment runs once on first entry; the goto skips it on repeat.
            if pc in self._backward_branch_targets:
                if self.X in ('k', 'j'):
                    self._ensure_mutable_x(self._cur_x_type)
                if self.Y in ('k', 'j') and self._simple(self.Y):
                    yn = self._alloc(self._cur_x_type)
                    self._emit(f'{yn} = {self.Y};')
                    self.Y = yn
                # If A holds a complex expression (e.g. ROM read), materialize it
                # before the loop header so the expression is evaluated once, not
                # on every iteration (PHA inside the loop would re-evaluate it).
                if self.A is not None and not self._simple(self.A):
                    self._materialize('A', self._cur_a_type)

            # Branch merge: emit assignments BEFORE the label so they only
            # execute on the fall-through path (goto skips them).
            # The branch variable then holds:
            # - The branch-source value when reached via goto (assignment skipped)
            # - The fall-through value when reached via fall-through (assigned here)
            #
            # No-fall-through case: if the immediately preceding instruction
            # was an unconditional transfer (BRA/JMP/RTS/RTL/RTI/tail-call),
            # there is no fall-through path into this label — control can
            # only arrive via the goto/branch that built the branch_state.
            # Using the current (post-BRA) register expressions as the
            # "fall-through value" emits bogus assignments that sit dead
            # between the goto and the label (and can, on a subsequent
            # pass, appear to double-update registers). Adopt the
            # branch_state's variables directly and skip the assignments.
            if self._prev_terminal and pc in self._branch_states:
                bs = self._branch_states.pop(pc)
                if bs.get('A_var'):
                    self.A = bs['A_var']
                if bs.get('X_var'):
                    self.X = bs['X_var']
                if bs.get('Y_var'):
                    self.Y = bs['Y_var']
                if bs.get('carry_var'):
                    self.carry = bs['carry_var']
                bstack = bs.get('stack')
                if bstack is not None:
                    self.stack = list(bstack)
            elif pc in self._branch_states:
                bs = self._branch_states.pop(pc)
                branch_a_var = bs.get('A_var')
                if branch_a_var and self.A != branch_a_var:
                    fall_a = self.A if self.A is not None else '0'
                    self._emit(f'{branch_a_var} = {fall_a};')
                    self.A = branch_a_var
                elif branch_a_var:
                    self.A = branch_a_var
                # X merge
                branch_x_var = bs.get('X_var')
                if branch_x_var and branch_x_var == self.Y and self.X != branch_x_var:
                    # X and Y shared the same var at branch time, but X has since
                    # diverged (e.g. TAX allocated a new X). Don't merge the new X
                    # value into the shared var — it would corrupt Y.
                    pass
                elif branch_x_var and self.X != branch_x_var:
                    fall_x = self.X if self.X is not None else '0'
                    self._emit(f'{branch_x_var} = {fall_x};')
                    self.X = branch_x_var
                elif branch_x_var:
                    self.X = branch_x_var
                # Y merge
                branch_y_var = bs.get('Y_var')
                if branch_y_var and self.Y != branch_y_var:
                    fall_y = self.Y if self.Y is not None else '0'
                    self._emit(f'{branch_y_var} = {fall_y};')
                    self.Y = branch_y_var
                elif branch_y_var:
                    self.Y = branch_y_var
                # Merge carry: if a carry_var was pre-allocated at the branch,
                # assign the fall-through carry to it before the label (so it
                # holds the correct value on both the branch-taken and fall-through
                # paths). This implements a phi node for the carry flag.
                carry_var = bs.get('carry_var')
                branch_carry = bs.get('carry')
                if carry_var:
                    if branch_carry != self.carry and self.carry is not None:
                        fall_carry = f'({self.carry}) ? 1 : 0' if not self._simple(self.carry) else self.carry
                        self._emit(f'{carry_var} = {fall_carry};')
                    self.carry = carry_var
                elif branch_carry is not None and branch_carry != self.carry:
                    # No pre-allocated var --can't easily merge, keep fall-through
                    pass
                # Stack merge: align the fall-through stack with the branch
                # path's stack so PHA/PLA pairs work across convergence points.
                # Truncate to the SHORTER stack (entries only on one path are
                # path-specific and must not leak to the other), then phi-merge
                # entries that differ.
                branch_stack = bs.get('stack')
                if branch_stack is not None:
                    fall_stack = self.stack
                    merged = []
                    merge_len = min(len(branch_stack), len(fall_stack))
                    for i in range(merge_len):
                        b_entry = branch_stack[i]
                        f_entry = fall_stack[i]
                        b_val = b_entry[1]
                        f_val = f_entry[1]
                        if b_val == f_val:
                            merged.append(b_entry)
                        elif b_val is not None and self._simple(b_val):
                            if f_val is not None:
                                self._emit(f'{b_val} = {f_val};')
                            merged.append((b_entry[0], b_val))
                        else:
                            phi = self._alloc(self._cur_a_type)
                            if f_val is not None:
                                self._emit(f'{phi} = {f_val};')
                            merged.append((b_entry[0], phi))
                    self.stack = merged
            self.lines.append(f'  label_{pc:04x}:;')
            # Branch target: clear dp_state so that dp reads re-read from g_ram.
            # Multiple paths can reach a label with different DP values cached,
            # so we must not use stale cached values after a merge point.
            self.dp_state.clear()
            if pc in self._backward_branch_targets:
                # Record A/X/Y at this label so a backward branch knows the
                # variable name each register must hold on loop re-entry.
                # (Forward branches handle merges via _branch_states, which
                # is set up at the branch site before the target is emitted.)
                if not hasattr(self, '_label_x'):
                    self._label_x = {}
                    self._label_y = {}
                    self._label_a = {}
                self._label_x[pc] = self.X
                self._label_y[pc] = self.Y
                self._label_a[pc] = self.A
                self._emit('WatchdogCheck();')

        # -- STZ ----------------------------------------------------------
        if mn == 'STZ':
            if mode == ABS and self._is_hw_reg(v):
                if wide_a:
                    self._emit(f'WriteRegWord(0x{v:x}, 0);')
                else:
                    self._emit(f'WriteReg(0x{v:x}, 0);')
            else:
                idx = {DP: '0', DP_X: self._idx('X'),
                       ABS: '0', ABS_X: self._idx('X')}.get(mode, '0')
                # Materialize any register that references this address
                # before zeroing it (prevents stale-read bugs like
                # LDA $90; AND #$0F; STZ $90; CMP #$08 reading 0).
                self._materialize_refs_to(self._wram(v, idx))
                if wide_a:
                    self._wram16_write(v, idx, '0')
                else:
                    self._emit(f'{self._wram(v, idx)} = 0;')
            self.carry_chain = None

        # -- LDA ----------------------------------------------------------
        elif mn == 'LDA':
            expr = self._resolve_mem(mode, v, wide=wide_a)
            if expr is not None:
                self.A = expr
            else:
                self.A = None
                self._warn(f'LDA {MODE_STR.get(mode,"?")} ${v:x} not handled')
            self.flag_src = self.A
            # LDA does NOT clear carry --carry persists for carry-chain patterns

        # -- STA ----------------------------------------------------------
        elif mn == 'STA':
            if self.A is None:
                self._warn(f'A unknown at STA ${v:x} --storing 0')
            # Materialize A before storing if it's a complex expression.
            # This prevents stale-reference bugs: if A = "g_ram[X] ^ 0x10"
            # and we write to g_ram[X], subsequent reads of A would re-evaluate
            # the expression with the NEW value (double-XOR bug).
            if self.A is not None and not self._simple(self.A):
                self._materialize('A', a_type)
            a = self.A if self.A is not None else '0'

            # Track DP writes for parameter passing
            if mode == DP:
                self.dp_state[v] = a

            if wide_a:
                self._emit_sta16(mode, v, a)
            else:
                self._emit_sta8(mode, v, a)

        # -- LDX ----------------------------------------------------------
        elif mn == 'LDX':
            expr = self._resolve_ldx(mode, v, wide_x, x_type)
            if expr is not None:
                name = self._alloc(x_type)
                self._emit(f'{name} = {expr};')
                self.X = name
            else:
                self.X = None
                self._warn(f'LDX {MODE_STR.get(mode,"?")} ${v:x} not handled')
            self.flag_src = self.X
            self.flag_width = 16 if wide_x else 8  # X-register flags

        # -- LDY ----------------------------------------------------------
        elif mn == 'LDY':
            expr = self._resolve_ldy(mode, v, wide_x, x_type)
            if expr is not None:
                name = self._alloc(x_type)
                self._emit(f'{name} = {expr};')
                self.Y = name
            else:
                self.Y = None
                self._warn(f'LDY {MODE_STR.get(mode,"?")} ${v:x} not handled')
            self.flag_src = self.Y
            self.flag_width = 16 if wide_x else 8  # X-register flags

        # -- STX ----------------------------------------------------------
        elif mn == 'STX':
            x = self._idx('X')
            if mode == DP: self.dp_state[v] = x
            if mode == ABS and self._is_hw_reg(v):
                if wide_x:
                    self._emit(f'WriteRegWord(0x{v:x}, {x});')
                else:
                    self._emit(f'WriteReg(0x{v:x}, {x});')
            elif wide_x and mode in (DP, ABS):
                self._materialize_refs_to(self._wram(v, '0'))
                self._wram16_write(v, '0', x)
            elif mode in (DP, ABS):
                self._materialize_refs_to(self._wram(v, '0'))
                self._emit(f'{self._wram(v, "0")} = {x};')
            elif mode == DP_Y:
                self._materialize_refs_to(self._wram(v, self._idx("Y")))
                self._emit(f'{self._wram(v, self._idx("Y"))} = {x};')
            else:
                self._emit(f'/* STX {MODE_STR.get(mode,"?")} ${v:x} */')

        # -- STY ----------------------------------------------------------
        elif mn == 'STY':
            y = self._idx('Y')
            if mode == DP: self.dp_state[v] = y
            if mode == ABS and self._is_hw_reg(v):
                if wide_x:
                    self._emit(f'WriteRegWord(0x{v:x}, {y});')
                else:
                    self._emit(f'WriteReg(0x{v:x}, {y});')
            elif wide_x and mode in (DP, ABS):
                self._materialize_refs_to(self._wram(v, '0'))
                self._wram16_write(v, '0', y)
            elif mode in (DP, ABS):
                self._materialize_refs_to(self._wram(v, '0'))
                self._emit(f'{self._wram(v, "0")} = {y};')
            elif mode == DP_X:
                self._emit(f'{self._wram(v, self._idx("X"))} = {y};')
            else:
                self._emit(f'/* STY {MODE_STR.get(mode,"?")} ${v:x} */')

        # -- Transfers ----------------------------------------------------
        elif mn == 'TAX':
            # 65816: TAX transfers the full 16-bit A register to X (subject to
            # X width). When X is 16-bit but A is 8-bit (m=1, x=0), the high
            # byte of X comes from the hidden B register, not zero-extension.
            # Idiom: JSR ReadByte; XBA; JSR ReadByte; TAX  builds a 16-bit X
            # from two 8-bit reads. Without the B merge the high byte is lost.
            if wide_x and not wide_a and self.B is not None and self.A is not None:
                tmp = self._alloc(x_type)
                self._emit(f'{tmp} = PAIR16({self.B}, {self.A});')
                self.X = tmp; self.flag_src = tmp
                self.B = None
            else:
                # If X is already a known mutable variable, write back into it so
                # loop accumulators (TXA; OP; TAX patterns) update the existing var.
                # BUT: if X and Y share the same variable (from TYX/TXY), writing
                # to X would corrupt Y. Also, if carry references X (from a prior
                # CPX), writing to X would corrupt the carry expression. In these
                # cases, allocate a new variable for X.
                x_shared_with_y = (self.X is not None and self.X == self.Y)
                carry_refs_x = (self.carry is not None and self.X is not None
                                and self.X in str(self.carry))
                if (self.X and self._simple(self.X) and self.X not in ('k', 'j')
                        and not x_shared_with_y and not carry_refs_x):
                    if self.A and not self._simple(self.A):
                        self._emit(f'{self.X} = {self.A};')
                        self.A = self.X
                    elif self.A:
                        self._emit(f'{self.X} = {self.A};')
                    self.flag_src = self.X
                else:
                    name = self._materialize('A', x_type)
                    self.X = name; self.flag_src = name
            self.flag_width = 16 if wide_x else 8  # X-register flags
        elif mn == 'TAY':
            # Same B-merge as TAX when Y is 16-bit and A is 8-bit.
            if wide_x and not wide_a and self.B is not None and self.A is not None:
                tmp = self._alloc(x_type)
                self._emit(f'{tmp} = PAIR16({self.B}, {self.A});')
                self.Y = tmp; self.flag_src = tmp
                self.B = None
            else:
                # If Y is already a known mutable variable, write back into it
                # (for TYA; OP; TAY loop accumulator patterns).
                y_shared_with_x = (self.Y is not None and self.Y == self.X)
                if self.Y and self._simple(self.Y) and self.Y not in ('k', 'j') and not y_shared_with_x:
                    if self.A is not None:
                        self._emit(f'{self.Y} = {self.A};')
                    self.flag_src = self.Y
                else:
                    # Y is None or a protected param — allocate a NEW variable
                    # (don't alias via _materialize, which would share the var with A).
                    if self.A is not None:
                        name = self._alloc(x_type)
                        self._emit(f'{name} = {self.A};')
                        self.Y = name
                    else:
                        self.Y = None
                    self.flag_src = self.Y
            self.flag_width = 16 if wide_x else 8  # X-register flags
        elif mn == 'TXA':
            # Always copy X to a new A variable. Aliasing (self.A = self.X)
            # causes bugs when X is later modified independently (e.g. by a
            # uint16-returning JSR) — A would see the changed value.
            if self.X is not None:
                name = self._alloc(a_type)
                self._emit(f'{name} = {self.X};')
                self.A = name
            else:
                self.A = None
            self.flag_src = self.A
        elif mn == 'TYA':
            # Always copy Y to a new A variable (same reason as TXA).
            if self.Y is not None:
                name = self._alloc(a_type)
                self._emit(f'{name} = {self.Y};')
                self.A = name
            else:
                self.A = None
            self.flag_src = self.A
        elif mn == 'TXY':
            # Copy X to new Y variable (don't alias — they may diverge)
            if self.X is not None:
                name = self._alloc(x_type)
                self._emit(f'{name} = {self.X};')
                self.Y = name
            else:
                self.Y = None
            self.flag_src = self.Y
            self.flag_width = 16 if wide_x else 8  # X-register flags
        elif mn == 'TYX':
            # Copy Y to new X variable (don't alias — they may diverge)
            if self.Y is not None:
                name = self._alloc(x_type)
                self._emit(f'{name} = {self.Y};')
                self.X = name
            else:
                self.X = None
            self.flag_src = self.X
            self.flag_width = 16 if wide_x else 8  # X-register flags

        # -- Stack push/pull ----------------------------------------------
        elif mn == 'PHX':
            # Save X value to a temp. PLX restores back into the SAME X variable.
            # Uses a single persistent save slot so loop re-entry reuses it.
            xn = self.X
            if xn is not None and self._simple(xn):
                if not hasattr(self, '_phx_save') or self._phx_save is None:
                    self._phx_save = self._alloc(self._cur_x_type)
                self._emit(f'{self._phx_save} = {xn};')
                self.stack.append(('X', self._phx_save, xn))
            else:
                self.stack.append(('X', xn, xn))
        elif mn == 'PHY':
            yn = self.Y
            if yn is not None and self._simple(yn):
                if not hasattr(self, '_phy_save') or self._phy_save is None:
                    self._phy_save = self._alloc(self._cur_x_type)
                self._emit(f'{self._phy_save} = {yn};')
                self.stack.append(('Y', self._phy_save, yn))
            else:
                self.stack.append(('Y', yn, yn))
        elif mn == 'PHA':
            # Materialize A before pushing --if A is a memory expression like
            # g_ram[0x1c], and the code modifies g_ram[0x1c] before PLA, the
            # pushed value would be stale. Snapshot into a variable.
            if self.A and not self._simple(self.A):
                self._materialize('A', a_type)
            self.stack.append(('A', self.A))
            self._last_pha_val = self.A  # remember for branch-forked PLA
        elif mn == 'PHP':
            # Save an immutable snapshot of the current flag state.
            # Instructions between PHP and PLP (e.g. LSR, LDY) may mutate the
            # variable that flag_src points to, so we snapshot it here.
            saved_flag = None
            if self.flag_src is not None:
                saved_flag = self._alloc(a_type)
                self._emit(f'{saved_flag} = {self.flag_src};')
            self.stack.append(('P', saved_flag, self.carry))

        elif mn == 'PLX':
            if self.stack:
                entry = self.stack.pop()
                save_var = entry[1]
                if entry[0] == 'X':
                    orig_var = entry[2] if len(entry) > 2 else save_var
                    if orig_var and save_var and self._simple(orig_var) and self._simple(save_var):
                        self._emit(f'{orig_var} = {save_var};')
                        self.X = orig_var
                    elif save_var:
                        self.X = save_var
                    else:
                        self.X = orig_var
                else:
                    # Cross-register pop (e.g. PHA then PLX): value goes A→stack→X
                    if save_var is not None:
                        self.X = save_var
                    else:
                        self.X = None
            else:
                if self._init_x is not None:
                    self.X = self._init_x
                    self._emit(f'/* PLX: stack empty --assuming {self._init_x} */')
                else:
                    self.X = None
                    self._emit('/* PLX: stack empty */')
        elif mn == 'PLY':
            if self.stack and self.stack[-1][0] == 'Y':
                entry = self.stack.pop()
                save_var = entry[1]
                orig_var = entry[2] if len(entry) > 2 else save_var
                if orig_var and save_var and self._simple(orig_var) and self._simple(save_var):
                    self._emit(f'{orig_var} = {save_var};')
                    self.Y = orig_var
                elif save_var:
                    self.Y = save_var
                else:
                    self.Y = orig_var
            elif self.stack:
                entry = self.stack.pop()
                self.Y = entry[1]
                self._emit(f'/* PLY: stack had {entry[0]}, using its value */')
            else:
                self.Y = None
                self._emit('/* PLY: stack empty */')
        elif mn == 'PLA':
            if self.stack and self.stack[-1][0] == 'A':
                self.A = self.stack.pop()[1]
            elif self.stack:
                # Stack type mismatch --pop whatever is there
                entry = self.stack.pop()
                self.A = entry[1]
                self._emit(f'/* PLA: stack had {entry[0]}, using its value */')
            elif hasattr(self, '_last_pha_val') and self._last_pha_val is not None:
                # Stack empty but we know what PHA pushed (branch-forked path)
                self.A = self._last_pha_val
                self._emit(f'/* PLA: stack empty, using last PHA value */')
            else:
                # PLA from empty stack = ReturnsTwice pattern (PLA PLA RTS).
                self.A = '0xff'
                self.Y = '0xff'
                self._emit('/* PLA: stack empty — ReturnsTwice skip-caller pattern */')
            self.flag_src = self.A
        elif mn == 'PLP':
            if self.stack and self.stack[-1][0] == 'P':
                _, saved_flag, saved_carry = self.stack.pop()
                # Restore the flag source and carry saved at PHP time.
                if saved_flag is not None:
                    self.flag_src = saved_flag
                if saved_carry is not None:
                    self.carry = saved_carry
            else:
                self._emit('/* PLP: stack mismatch */')
            self.overflow = None  # PLP restores V from stack

        # -- CLC / SEC ----------------------------------------------------
        elif mn == 'CLC':
            self.carry = '0'; self.carry_chain = None
        elif mn == 'SEC':
            self.carry = '1'

        # -- ADC (all modes) --HANDOFF requirement B ----------------------
        elif mn == 'ADC':
            self._emit_adc(mode, v, wide_a, a_type)
            self.overflow = None  # ADC modifies V flag

        # -- SBC ----------------------------------------------------------
        elif mn == 'SBC':
            self._emit_sbc(mode, v, wide_a, a_type)
            self.overflow = None  # SBC modifies V flag

        # -- AND / ORA / EOR ----------------------------------------------
        elif mn == 'AND':
            self._emit_logic('&', mode, v, wide_a)
        elif mn == 'ORA':
            self._emit_logic('|', mode, v, wide_a)
        elif mn == 'EOR':
            self._emit_logic('^', mode, v, wide_a)

        # -- CMP ----------------------------------------------------------
        elif mn == 'CMP':
            a = self._wrap(self.A) if self.A else '0'
            mem = self._resolve_mem(mode, v, wide=wide_a)
            if mem is not None:
                self.flag_src = f'{a} - {mem}'
                self.flag_width = 16 if wide_a else 8
                self.carry = f'({a} >= {mem})'
            else:
                self.flag_src = None
                self._emit(f'/* CMP {MODE_STR.get(mode,"?")} ${v:x} */')

        # -- CPX ----------------------------------------------------------
        elif mn == 'CPX':
            x = self._wrap(self.X) if self.X else '0'
            mem = self._resolve_mem(mode, v, wide=wide_x)
            if mem is not None:
                self.flag_src = f'{x} - {mem}'
                self.flag_width = 16 if wide_x else 8
                self.carry = f'({x} >= {mem})'
            else:
                self.flag_src = None
                self._emit(f'/* CPX {MODE_STR.get(mode,"?")} ${v:x} */')

        # -- CPY ----------------------------------------------------------
        elif mn == 'CPY':
            y = self._wrap(self.Y) if self.Y else '0'
            mem = self._resolve_mem(mode, v, wide=wide_x)
            if mem is not None:
                self.flag_src = f'{y} - {mem}'
                self.flag_width = 16 if wide_x else 8
                self.carry = f'({y} >= {mem})'
            else:
                self.flag_src = None
                self._emit(f'/* CPY {MODE_STR.get(mode,"?")} ${v:x} */')

        # -- INC / DEC register -------------------------------------------
        elif mn == 'INX':
            xn = self._ensure_mutable_x(x_type)
            if xn:
                self._materialize_refs_to(xn)
                self._emit(f'{xn}++;')
                self.flag_src = xn
                self.flag_width = 16 if wide_x else 8
            else:
                self._emit('/* INX on unknown X */')
        elif mn == 'DEX':
            xn = self._ensure_mutable_x(x_type)
            if xn:
                self._materialize_refs_to(xn)
                self._emit(f'{xn}--;')
                self.flag_src = xn
                self.flag_width = 16 if wide_x else 8
            else:
                self._emit('/* DEX on unknown X */')
        elif mn == 'INY':
            yn = self.Y
            if yn and self._simple(yn):
                self._materialize_refs_to(yn)
                self._emit(f'{yn}++;')
                self.flag_src = yn
                self.flag_width = 16 if wide_x else 8
            else:
                self._emit('/* INY on unknown Y */')
        elif mn == 'DEY':
            yn = self.Y
            if yn and self._simple(yn):
                self._materialize_refs_to(yn)
                self._emit(f'{yn}--;')
                self.flag_src = yn
                self.flag_width = 16 if wide_x else 8
            else:
                self._emit('/* DEY on unknown Y */')

        elif mn == 'INC' and mode == ACC:
            an = self._materialize('A', a_type)
            self._emit(f'{an}++;')
            self.flag_src = an
        elif mn == 'DEC' and mode == ACC:
            an = self._materialize('A', a_type)
            self._emit(f'{an}--;')
            self.flag_src = an

        # -- INC / DEC memory ---------------------------------------------
        elif mn == 'INC':
            if wide_a and mode in (DP, ABS):
                # 16-bit INC: operate on word
                idx = '0'
                w16 = self._wram16(v, idx)
                tmp = self._alloc('uint16')
                self._emit(f'{tmp} = {w16} + 1;')
                self._wram16_write(v, idx, tmp)
                self.flag_src = tmp
                if mode == DP:
                    self.dp_state.pop(v, None)
                    self.dp_state.pop(v + 1, None)
            else:
                mem = self._resolve_mem_rw(mode, v)
                if mem:
                    self._emit(f'{mem}++;')
                    self.flag_src = mem
                    if mode == DP:
                        self.dp_state.pop(v, None)  # invalidate stale dp_state after INC
                else:
                    self._emit(f'/* INC {MODE_STR.get(mode,"?")} ${v:x} */')
        elif mn == 'DEC':
            if wide_a and mode in (DP, ABS):
                # 16-bit DEC: operate on word
                idx = '0'
                w16 = self._wram16(v, idx)
                tmp = self._alloc('uint16')
                self._emit(f'{tmp} = {w16} - 1;')
                self._wram16_write(v, idx, tmp)
                self.flag_src = tmp
                if mode == DP:
                    self.dp_state.pop(v, None)
                    self.dp_state.pop(v + 1, None)
            else:
                mem = self._resolve_mem_rw(mode, v)
                if mem:
                    self._emit(f'{mem}--;')
                    self.flag_src = mem
                    if mode == DP:
                        self.dp_state.pop(v, None)  # invalidate stale dp_state after DEC
                else:
                    self._emit(f'/* DEC {MODE_STR.get(mode,"?")} ${v:x} */')

        # -- ASL ----------------------------------------------------------
        elif mn == 'ASL':
            if mode == ACC:
                an = self._materialize('A', a_type)
                cv = self._alloc_tmp('uint8')
                self._emit(f'{cv} = ({an} >> 7) & 1;')
                self.carry = cv
                self._emit(f'{an} <<= 1;')
                self.flag_src = an
            else:
                mem = self._resolve_mem_rw(mode, v)
                if mem:
                    cv = self._alloc_tmp('uint8')
                    self._emit(f'{cv} = ({mem} >> 7) & 1;')
                    self.carry = cv
                    self._emit(f'{mem} <<= 1;')
                    self.flag_src = None
                else:
                    self._emit(f'/* ASL {MODE_STR.get(mode,"?")} ${v:x} */')

        # -- LSR ----------------------------------------------------------
        elif mn == 'LSR':
            if mode == ACC:
                an = self._materialize('A', a_type)
                cv = self._alloc_tmp('uint8')
                self._emit(f'{cv} = {an} & 1;')
                self.carry = cv
                self._emit(f'{an} >>= 1;')
                self.flag_src = an
            else:
                mem = self._resolve_mem_rw(mode, v)
                if mem:
                    cv = self._alloc_tmp('uint8')
                    self._emit(f'{cv} = {mem} & 1;')
                    self.carry = cv
                    self._emit(f'{mem} >>= 1;')
                    self.flag_src = None
                else:
                    self._emit(f'/* LSR {MODE_STR.get(mode,"?")} ${v:x} */')

        # -- ROL ----------------------------------------------------------
        elif mn == 'ROL':
            carry_in = self.carry if self.carry else '0'
            if mode == ACC:
                an = self._materialize('A', a_type)
                cv = self._alloc_tmp('uint8')
                self._emit(f'{cv} = ({an} >> 7) & 1;')
                self._emit(f'{an} = ({a_type})(({an} << 1) | {carry_in});')
                self.carry = cv; self.flag_src = an
            else:
                mem = self._resolve_mem_rw(mode, v)
                if mem:
                    cv = self._alloc_tmp('uint8')
                    self._emit(f'{cv} = ({mem} >> 7) & 1;')
                    self._emit(f'{mem} = (uint8)(({mem} << 1) | {carry_in});')
                    self.carry = cv; self.flag_src = None
                else:
                    self._emit(f'/* ROL {MODE_STR.get(mode,"?")} ${v:x} */')

        # -- ROR ----------------------------------------------------------
        elif mn == 'ROR':
            carry_in = self.carry if self.carry else '0'
            if mode == ACC:
                an = self._materialize('A', a_type)
                cv = self._alloc_tmp('uint8')
                self._emit(f'{cv} = {an} & 1;')
                self._emit(f'{an} = ({a_type})(({an} >> 1) | ({carry_in} << 7));')
                self.carry = cv; self.flag_src = an
            else:
                mem = self._resolve_mem_rw(mode, v)
                if mem:
                    cv = self._alloc_tmp('uint8')
                    self._emit(f'{cv} = {mem} & 1;')
                    self._emit(f'{mem} = (uint8)(({mem} >> 1) | ({carry_in} << 7));')
                    self.carry = cv; self.flag_src = None
                else:
                    self._emit(f'/* ROR {MODE_STR.get(mode,"?")} ${v:x} */')

        # -- BIT ----------------------------------------------------------
        elif mn == 'BIT':
            a = self._wrap(self.A) if self.A else '0'
            mem = self._resolve_mem(mode, v, wide=wide_a)
            if mem is not None:
                self.flag_src = f'{a} & {mem}'
                # BIT sets V flag from bit 6 of the memory operand (not the AND result)
                if mode != IMM:  # BIT #imm does NOT affect V on 65816
                    self.overflow = f'({mem}) & 0x40'
            else:
                self.flag_src = None

        # -- TSB / TRB ----------------------------------------------------
        elif mn == 'TSB':
            a = self._wrap(self.A) if self.A else '0'
            mem = self._resolve_mem_rw(mode, v)
            if mem:
                self._emit(f'{mem} |= {a};')
                self.flag_src = None
            else:
                self._emit(f'/* TSB {MODE_STR.get(mode,"?")} ${v:x} */')
        elif mn == 'TRB':
            a = self._wrap(self.A) if self.A else '0'
            mem = self._resolve_mem_rw(mode, v)
            if mem:
                self._emit(f'{mem} &= ~{a};')
                self.flag_src = None
            else:
                self._emit(f'/* TRB {MODE_STR.get(mode,"?")} ${v:x} */')

        # -- Branches -----------------------------------------------------
        elif mn in ('BPL','BMI','BEQ','BNE','BCC','BCS','BVS','BVC','BRA','BRL'):
            self._emit_branch(mn, v)

        # -- JMP ----------------------------------------------------------
        elif mn == 'JMP':
            # JMP LONG (JML) with dispatch table: route through _emit_call
            # so the dispatch table entries are emitted as a switch/function table.
            if mode == LONG and insn.dispatch_entries:
                self._emit_call(insn)
            else:
                self._emit_jmp(mode, v)

        # -- JSL / JSR ----------------------------------------------------
        elif mn in ('JSL', 'JSR'):
            self._emit_call(insn)

        # -- RTL / RTS ----------------------------------------------------
        elif mn in ('RTL', 'RTS'):
            # Pair RecompStackPush (emitted on function entry) with a pop so
            # g_recomp_stack reflects the real dynamic call chain — required
            # for the watchdog dump to name the caller of a hung routine.
            self._emit('RecompStackPop();')
            self._emit_return_for_current_sig()

        elif mn == 'RTI':
            self._emit('RecompStackPop();')
            self._emit('return;  /* RTI */')

        # -- REP / SEP (tracked by decoder, emit PAIR16 merge on REP #$20) --
        elif mn in ('REP', 'SEP'):
            if mn == 'REP' and (v & 0x20):
                # Switching to 16-bit accumulator. If we have both A (lo) and B (hi)
                # from the LDA hi,X; XBA; LDA lo,X idiom, merge into PAIR16(B, A).
                if self.A is not None and self.B is not None:
                    merged = self._alloc('uint16')
                    self._emit(f'{merged} = PAIR16({self.B}, {self.A});')
                    self.A = merged
                    self.B = None
                    self.flag_src = merged
                elif self.A is not None:
                    # Only A known --zero-extend to uint16
                    merged = self._alloc('uint16')
                    self._emit(f'{merged} = (uint16){self.A};')
                    self.A = merged
                    self.B = None
                    self.flag_src = merged
            if mn == 'REP' and (v & 0x10):
                # Switching to 16-bit index registers. Promote existing X/Y
                # variables from uint8 to uint16 so INX/INY/CPX/CPY use the
                # correct width and don't wrap at 256.
                for reg in ('X', 'Y'):
                    val = getattr(self, reg)
                    if val is not None and val in self._hoisted and self._hoisted[val] == 'uint8':
                        self._hoisted[val] = 'uint16'
            elif mn == 'SEP' and (v & 0x20):
                # Switching back to 8-bit accumulator. The high byte of A moves
                # to the hidden B register. Preserve it so a subsequent XBA can
                # recover the high byte (common pattern: REP; 16-bit math; SEP;
                # STA lo; XBA; STA hi).
                if self.A is not None:
                    # Use (uint8)(x >> 8) instead of HIBYTE() which requires an lvalue
                    self.B = f'(uint8)(({self.A}) >> 8)'
                else:
                    self.B = None

        # -- XBA ----------------------------------------------------------
        elif mn == 'XBA':
            if wide_a and self.A is not None:
                # 16-bit mode: XBA byte-swaps the 16-bit accumulator.
                tmp = self._alloc('uint16')
                self._emit(f'{tmp} = swap16({self.A});')
                self.A = tmp
            else:
                # 8-bit mode: swap A (low byte) and B (high byte).
                # Common idiom: LDA hi,X; XBA; LDA lo,X; REP #$20 -> PAIR16(hi, lo)
                self.A, self.B = self.B, self.A
            self.flag_src = self.A

        # -- No-ops -------------------------------------------------------
        elif mn in ('NOP', 'CLD', 'SED', 'CLI', 'SEI', 'XCE'):
            pass
        elif mn == 'CLV':
            self.overflow = None
        elif mn in ('PHB', 'PLB', 'PHK', 'PHD', 'PLD'):
            pass
        elif mn == 'TSX':
            self.X = None; self.flag_src = None
        elif mn == 'TXS':
            pass
        elif mn in ('TCD', 'TDC', 'TCS', 'TSC'):
            pass
        elif mn in ('BRK', 'COP', 'WDM', 'STP', 'WAI'):
            self._emit(f'/* {mn} --should not execute */')
        elif mn in ('PEI', 'PEA', 'PER'):
            self._emit(f'/* {mn} --not implemented */')

        # -- MVN / MVP (block move) --------------------------------------
        elif mn in ('MVN', 'MVP'):
            dst_bank = v & 0xFF
            src_bank = (v >> 8) & 0xFF
            x_val = self.X if self.X is not None else '0'
            y_val = self.Y if self.Y is not None else '0'
            a_val = self.A if self.A is not None else '0'
            count = f'(uint16)({a_val}) + 1'
            # Source / dest pointer: use MvnPtr so 65816 LoROM WRAM-mirror
            # banks ($00-$3F, $80-$BF) with addr<$2000 resolve correctly at
            # runtime. $7E/$7F are direct WRAM; everything else is ROM.
            src_expr = f'MvnPtr(0x{src_bank:02x}, {x_val})'
            dst_expr = f'MvnPtr(0x{dst_bank:02x}, {y_val})'
            if mn == 'MVP':
                # MVP: X/Y point to END of block; adjust to start
                self._emit(f'MemCpy({dst_expr} - (uint16)({a_val}), {src_expr} - (uint16)({a_val}), {count});')
            else:
                self._emit(f'MemCpy({dst_expr}, {src_expr}, {count});')
            # After block move: registers are consumed
            self.A = None; self.X = None; self.Y = None
            self.flag_src = None

        # -- Unhandled ----------------------------------------------------
        else:
            self._warn(f'Unhandled: {insn}',
                       f'Add handler for {mn} mode={MODE_STR.get(mode, mode)}')

        # -- Track whether this instruction terminates fall-through ------
        # Used by the next instruction's label-merge step so that register
        # state from a now-dead path doesn't leak into a label reachable
        # only via goto/branch.
        if mn in ('RTS', 'RTL', 'RTI', 'BRK', 'STP', 'BRA', 'BRL'):
            self._prev_terminal = True
        elif mn == 'JMP' and mode in (ABS, LONG, INDIR, INDIR_X, INDIR_L):
            # All JMP/JML variants are unconditional transfers; any next
            # linear instruction is unreachable without an incoming branch.
            self._prev_terminal = True
        else:
            self._prev_terminal = False

    # -- Sub-emitters ---------------------------------------------------------

    def _resolve_mem_rw(self, mode: int, v: int) -> Optional[str]:
        """Resolve a read-modify-write memory operand (for INC/DEC/ASL/LSR/ROL/ROR).
        Note: hardware registers (INC $21xx, DEC $43xx) are rare and typically
        only meaningful for WRAM-mirrored addresses. We still route through g_ram
        here because read-modify-write on true MMIO is unusual and g_ram serves as
        the shadow register storage."""
        if mode in (DP, ABS):
            return self._wram(v, '0')
        elif mode in (DP_X, ABS_X):
            return self._wram(v, self._idx('X'))
        elif mode in (DP_Y,):
            return self._wram(v, self._idx('Y'))
        return None

    def _resolve_ldx(self, mode: int, v: int, wide: bool, type_: str) -> Optional[str]:
        if mode == IMM:
            return str(v) if v < 10 else f'0x{v:x}'
        elif mode == DP:
            return self._wram16(v, '0') if wide else f'g_ram[0x{v:x}]'
        elif mode == ABS:
            if v >= 0x8000:
                full = (self.bank << 16) | v
                return self._rom16(full, '0') if wide else self._rom(full, '0')
            if self._is_hw_reg(v):
                return f'ReadRegWord(0x{v:x})' if wide else f'ReadReg(0x{v:x})'
            return self._wram16(v, '0') if wide else f'g_ram[0x{v:x}]'
        elif mode == DP_Y:
            return self._wram16(v, self._idx('Y')) if wide else self._wram(v, self._idx('Y'))
        elif mode == ABS_Y:
            if v >= 0x8000:
                full = (self.bank << 16) | v
                return self._rom16(full, self._idx('Y')) if wide else self._rom(full, self._idx('Y'))
            if self._is_hw_reg(v):
                y = self._idx('Y')
                return f'ReadRegWord(0x{v:x} + {y})' if wide else f'ReadReg(0x{v:x} + {y})'
            return self._wram16(v, self._idx('Y')) if wide else self._wram(v, self._idx('Y'))
        return None

    def _resolve_ldy(self, mode: int, v: int, wide: bool, type_: str) -> Optional[str]:
        if mode == IMM:
            return str(v) if v < 10 else f'0x{v:x}'
        elif mode == DP:
            return self._wram16(v, '0') if wide else f'g_ram[0x{v:x}]'
        elif mode == ABS:
            if v >= 0x8000:
                full = (self.bank << 16) | v
                return self._rom16(full, '0') if wide else self._rom(full, '0')
            if self._is_hw_reg(v):
                return f'ReadRegWord(0x{v:x})' if wide else f'ReadReg(0x{v:x})'
            return self._wram16(v, '0') if wide else f'g_ram[0x{v:x}]'
        elif mode == DP_X:
            return self._wram16(v, self._idx('X')) if wide else self._wram(v, self._idx('X'))
        elif mode == ABS_X:
            if v >= 0x8000:
                full = (self.bank << 16) | v
                return self._rom16(full, self._idx('X')) if wide else self._rom(full, self._idx('X'))
            return self._wram16(v, self._idx('X')) if wide else self._wram(v, self._idx('X'))
        return None

    def _check_dp_sync(self, dp_addr: int):
        """ORACLE BRIDGE: emit sync call if dp_addr is in the dp_sync map.
        Remove this method when all banks are recompiled and oracle is gone."""
        if dp_addr in self.dp_sync:
            self._emit(f'{self.dp_sync[dp_addr]}();  /* ORACLE BRIDGE: dp_sync */')

    def _emit_sta8(self, mode: int, v: int, a: str):
        if   mode == DP:
            self._emit(f'{self._wram(v, "0")} = {a};')
            self._check_dp_sync(v)
        elif mode == DP_X:     self._emit(f'{self._wram(v, self._idx("X"))} = {a};')
        elif mode == DP_Y:     self._emit(f'{self._wram(v, self._idx("Y"))} = {a};')
        elif mode == ABS:
            if self._is_hw_reg(v):
                self._emit(f'WriteReg(0x{v:x}, {a});')
            else:
                self._emit(f'g_ram[0x{v:x}] = {a};')
        elif mode == ABS_X:
            if self._is_hw_reg(v):
                self._emit(f'WriteReg(0x{v:x} + {self._idx("X")}, {a});')
            else:
                self._emit(f'{self._wram(v, self._idx("X"))} = {a};')
        elif mode == ABS_Y:
            if self._is_hw_reg(v):
                self._emit(f'WriteReg(0x{v:x} + {self._idx("Y")}, {a});')
            else:
                self._emit(f'{self._wram(v, self._idx("Y"))} = {a};')
        elif mode == INDIR_LY: self._indir_write(v, self._idx('Y'), a)
        elif mode == INDIR_L:  self._indir_write(v, '0', a)
        elif mode == INDIR_Y:
            # STA ($dp),Y — 16-bit indirect (NOT long). Write to WRAM via 2-byte pointer.
            addr_expr = f'(g_ram[0x{v:02x}] | (g_ram[0x{v:02x} + 1] << 8))'
            y_expr = self._idx('Y')
            self._emit(f'g_ram[{addr_expr} + {y_expr}] = {a};')
        elif mode == INDIR_DPX:
            addr_expr = f'(g_ram[0x{v:02x} + {self._idx("X")}] | (g_ram[0x{v:02x} + {self._idx("X")} + 1] << 8))'
            self._emit(f'g_ram[{addr_expr}] = {a};')
        elif mode == DP_INDIR:
            self._emit(f'g_ram[{self._dp_indir_addr(v)}] = {a};')
        elif mode == LONG:
            bk = (v >> 16) & 0xFF
            if bk in (0x7E, 0x7F):
                addr = v & 0xFFFF
                base = addr if bk == 0x7E else (0x10000 + addr)
                self._emit(f'g_ram[0x{base:x}] = {a};')
            elif bk == 0x70:
                addr = v & 0xFFFF
                self._emit(f'g_sram[0x{addr:x}] = {a};')
            else:
                self._emit(f'// STA long ${v:06x} --ROM, ignored')
        elif mode == LONG_X:
            bk = (v >> 16) & 0xFF
            if bk in (0x7E, 0x7F):
                addr = v & 0xFFFF
                base = addr if bk == 0x7E else (0x10000 + addr)
                self._emit(f'{self._wram(base, self._idx("X"))} = {a};')
            elif bk == 0x70:
                addr = v & 0xFFFF
                self._emit(f'g_sram[0x{addr:x} + {self._idx("X")}] = {a};')
            else:
                self._emit(f'// STA long,x ${v:06x} --ROM, ignored')
        elif mode == STK_IY:
            self._emit(f'/* STA (stk,S),Y ${v:02x} --not implemented */')
        else:
            self._emit(f'/* STA {MODE_STR.get(mode,"?")} ${v:x} */')

    def _emit_sta16(self, mode: int, v: int, a: str):
        if   mode == DP:
            self._wram16_write(v, '0', a)
            self._check_dp_sync(v)
            self._check_dp_sync(v + 1)  # 16-bit write touches v and v+1
        elif mode == DP_X:     self._wram16_write(v, self._idx('X'), a)
        elif mode == ABS:
            if self._is_hw_reg(v):
                self._emit(f'WriteRegWord(0x{v:x}, {a});')
            else:
                self._wram16_write(v, '0', a)
        elif mode == ABS_X:
            if self._is_hw_reg(v):
                self._emit(f'WriteRegWord(0x{v:x} + {self._idx("X")}, {a});')
            else:
                self._wram16_write(v, self._idx('X'), a)
        elif mode == ABS_Y:
            if self._is_hw_reg(v):
                self._emit(f'WriteRegWord(0x{v:x} + {self._idx("Y")}, {a});')
            else:
                self._wram16_write(v, self._idx('Y'), a)
        elif mode == INDIR_LY: self._indir_write(v, self._idx('Y'), a)
        elif mode == INDIR_L:  self._indir_write(v, '0', a)
        elif mode == LONG:
            bk = (v >> 16) & 0xFF
            if bk in (0x7E, 0x7F):
                addr = v & 0xFFFF
                base = addr if bk == 0x7E else (0x10000 + addr)
                self._wram16_write(base, '0', a)
            elif bk == 0x70:
                addr = v & 0xFFFF
                self._emit(f'*(uint16*)(g_sram + 0x{addr:x}) = {a};')
            else:
                self._emit(f'// STA16 long ${v:06x} --ROM, ignored')
        elif mode == LONG_X:
            bk = (v >> 16) & 0xFF
            if bk in (0x7E, 0x7F):
                addr = v & 0xFFFF
                base = addr if bk == 0x7E else (0x10000 + addr)
                self._wram16_write(base, self._idx('X'), a)
            elif bk == 0x70:
                addr = v & 0xFFFF
                self._emit(f'*(uint16*)(g_sram + 0x{addr:x} + {self._idx("X")}) = {a};')
            else:
                self._emit(f'// STA16 long,x ${v:06x} --ROM, ignored')
        else:
            self._emit(f'/* STA16 {MODE_STR.get(mode,"?")} ${v:x} */')

    def _emit_adc(self, mode: int, v: int, wide: bool, a_type: str):
        """ADC --handles carry chain propagation for ALL addressing modes.

        HANDOFF requirement B: The carry from the first ADC MUST propagate
        to the second ADC #$00 regardless of the first ADC's addressing mode.
        """
        mem = self._resolve_mem(mode, v, wide=wide)
        if mem is None:
            self.A = None
            self._warn(f'ADC {MODE_STR.get(mode,"?")} ${v:x} not handled')
            self.flag_src = self.A
            self.carry_chain = None
            return

        if self.A is None:
            self._warn(f'A unknown at ADC --using 0')
            an = '0'
        elif not self._simple(self.A):
            an = self._materialize('A', a_type)
        else:
            an = self.A

        if mode == IMM and v == 0 and self.carry_chain:
            # ADC #0 after a carry chain: propagate carry high byte
            chain = self.carry_chain
            carry_hi = f'({a_type})(({chain["var"]}) >> 8)'
            a_inner = self._wrap(an)
            self.A = f'{a_inner} + {carry_hi}'
            self.carry_chain = None
            # Carry out from the propagated ADC #0 is the overflow of the sum
            self.carry = f'({chain["var"]} >= 256)' if not wide else f'({chain["var"]} >= 65536)'
        else:
            # Start a new carry chain --works for ALL modes (IMM, DP, ABS, LONG, etc.)
            widen = 'uint32' if wide else 'uint16'
            # Include carry input when non-zero (e.g. after PHP/PLP restores carry).
            if self.carry and self.carry != '0':
                carry_in = f'(({self.carry}) ? 1 : 0)'
                chain_expr = f'({widen}){an} + {mem} + {carry_in}'
            else:
                chain_expr = f'({widen}){an} + {mem}'
            tname = self._alloc_tmp(widen)
            self._emit(f'{tname} = {chain_expr};')
            self.carry_chain = {'var': tname, 'expr': chain_expr}
            self.A = f'({a_type})({tname})'
            # Carry out: set when the wider result overflows the original type
            threshold = 65536 if wide else 256
            self.carry = f'({tname} >= {threshold})'
        self.flag_src = self.A

    def _emit_sbc(self, mode: int, v: int, wide: bool, a_type: str):
        """SBC --handles borrow chain propagation for multi-word subtraction.

        65816 SBC: A = A - operand - (1 - carry).
        After SEC (carry=1): A = A - operand (no borrow input).
        After a previous SBC: borrow from the low word propagates via carry.
        """
        mem = self._resolve_mem(mode, v, wide=wide)
        if mem is None:
            self.A = None
            self._warn(f'SBC {MODE_STR.get(mode,"?")} ${v:x} not handled')
            self.flag_src = self.A
            self.carry_chain = None
            return

        if self.A is None:
            self._warn(f'A unknown at SBC --using 0')
            an = '0'
        elif not self._simple(self.A):
            an = self._materialize('A', a_type)
        else:
            an = self.A

        # SBC with borrow: A = A - operand - (1 - carry)
        # When carry=1 (SEC or no borrow from previous): plain subtraction
        # When carry=0 (borrow from previous): subtract extra 1
        if self.carry and self.carry != '1':
            # Chained SBC: include borrow from previous operation
            borrow = f'(({self.carry}) ? 0 : 1)'
            self.A = f'({a_type})({an} - {mem} - {borrow})'
            # Carry out: 1 if no borrow occurred (result >= 0 in unsigned terms)
            # Use wider type to detect underflow
            widen = 'uint32' if wide else 'uint16'
            tname = self._alloc_tmp(widen)
            self._emit(f'{tname} = ({widen}){an} - {mem} - {borrow};')
            self.A = f'({a_type})({tname})'
            threshold = 65536 if wide else 256
            self.carry = f'(({tname}) < {threshold})'  # no borrow if result fits
        else:
            # First SBC after SEC (carry=1): simple subtraction
            self.A = f'({a_type})({an} - {mem})'
            self.carry = f'(({a_type}){an} >= ({a_type}){mem})'
        self.flag_src = self.A
        self.carry_chain = None

    def _emit_logic(self, op: str, mode: int, v: int, wide: bool):
        """AND / ORA / EOR --all modes."""
        mem = self._resolve_mem(mode, v, wide=wide)
        if mem is None:
            self.A = None
            self._warn(f'{op} {MODE_STR.get(mode,"?")} ${v:x} not handled')
        else:
            a = self._wrap(self.A) if self.A else '0'
            self.A = f'{a} {op} {mem}'
        self.flag_src = self.A

    def _is_always_taken(self, mn: str) -> bool:
        """Check if a conditional branch is always taken (flag_src is a known constant)."""
        fs = self.flag_src
        if fs is None:
            return False
        try:
            val = int(fs)
            if mn == 'BNE': return val != 0
            if mn == 'BEQ': return val == 0
            if mn == 'BPL': return (val & 0x80) == 0
            if mn == 'BMI': return (val & 0x80) != 0
        except (ValueError, TypeError):
            pass
        return False

    def _emit_branch(self, mn: str, v: int):
        # Detect always-taken branches (e.g. LDX #$01; BNE)
        if self._is_always_taken(mn) and v in self.valid_branch_targets:
            self._emit(f'goto label_{v:04x};  /* {mn} always taken */')
            return

        cond = self._branch_cond(mn)
        if v not in self.valid_branch_targets:
            is_unconditional = mn in ('BRA', 'BRL')
            if self._emit_tail_call(v, cond=None if is_unconditional else cond):
                return
            # Check if target is a shared RTS/RTL --if so, treating as return is correct
            is_shared_rts = False
            if self._rom_bytes is not None:
                try:
                    off = lorom_offset(self.bank, v)
                    opcode = self._rom_bytes[off]
                    is_shared_rts = opcode in (0x60, 0x6B)  # RTS or RTL
                except (AssertionError, IndexError):
                    pass
            if not is_shared_rts:
                # BRA/BRL/BCC/etc. with operand below $8000 points into
                # WRAM or RAM mirror — LoROM has no code there. That's
                # always a decoder artifact (a data byte consumed as
                # the opcode's offset). Swallow silently with a
                # comment instead of a noisy "add cfg hint" warning,
                # since there's nothing the cfg could usefully name.
                if v < 0x8000:
                    self._emit(f'/* {mn} ${v:04X} unreachable: target < $8000 is RAM, not code */')
                else:
                    reason = 'before func start' if v < (self.func_start & 0xFFFF) else 'outside decoded range'
                    self._warn(f'{mn} ${v:04X} treated as return --{reason}',
                               f"Add 'end:{v:04X}' or 'name {(self.bank<<16)|v:06X} <Name>' to cfg")
            # Branch-as-return: pair with the entry's RecompStackPush.
            rv = self._return_value_expr()
            if rv is None:
                ret_expr = 'RecompStackPop(); return;'
            else:
                ret_expr = f'RecompStackPop(); return {rv};'
            if mn in ('BRA', 'BRL'):
                self._emit(ret_expr)
            else:
                self._emit(f'if ({cond}) {{ {ret_expr} }}')
        elif mn in ('BRA', 'BRL'):
            # Check if unconditional branch targets past function end
            # Skip tail call if target was decoded (has a label in this function)
            if self.end_addr and v >= self.end_addr and v not in self.valid_branch_targets:
                if self._emit_tail_call(v):
                    return
            # Save branch state for phi merge at the target label.
            # BRA is unconditional but the target may also be reached by
            # fall-through from a conditional branch (e.g. BEQ skip; ...;
            # BRA merge; skip: ...; merge:). Without this, the BRA path's
            # A value is lost at the merge point.
            if v in self.valid_branch_targets and self.A is not None:
                branch_a = self._materialize('A', self._cur_a_type)
                existing = self._branch_states.get(v)
                if existing and existing.get('A_var') and branch_a != existing['A_var']:
                    self._emit(f'{existing["A_var"]} = {self.A};')
                    branch_a = existing['A_var']
                self._branch_states[v] = {
                    'A_var': branch_a, 'X_var': None, 'Y_var': None,
                    'carry': self.carry, 'carry_var': None,
                    'stack': list(self.stack),
                }
            # Backward BRA: if the target label was emitted earlier with a
            # known A/X/Y variable, assign the current register values into
            # those variables before the goto so the next loop iteration
            # sees the updated register state. X/Y already handled below;
            # we also need A for patterns like HexToDec's SBC-A; BRA loop.
            if hasattr(self, '_label_a') and v in self._label_a:
                la = self._label_a[v]
                if la and self.A and la != self.A and self._simple(la):
                    self._emit(f'{la} = {self.A};')
            if hasattr(self, '_label_x') and v in self._label_x:
                lx = self._label_x[v]
                ly = self._label_y.get(v)
                if lx and self.X and lx != self.X and self._simple(lx) and self._simple(self.X):
                    if lx != ly:
                        self._emit(f'{lx} = {self.X};')
                if ly and self.Y and ly != self.Y and self._simple(ly) and self._simple(self.Y):
                    if ly != lx:
                        self._emit(f'{ly} = {self.Y};')
            self._emit(f'goto label_{v:04x};')
        else:
            # Branch merge: materialize registers before the branch so the
            # branch path preserves values in C variables. The fall-through
            # path may overwrite registers. At the target label, we assign
            # fall-through values to the branch variables, so C variables
            # have correct values on either path.
            branch_a = None
            branch_x = None
            branch_y = None
            carry_var = None
            # For out-of-range backward targets (address below function start),
            # the label is emitted in dead-code position (after the fall-through
            # path's return). The goto skips any pre-label code, so
            # _ensure_mutable_x at the label site would be dead.  Pre-materialize
            # X HERE at the branch site so both branch-taken and fall-through
            # paths see a mutable (non-parameter) X variable.
            if (v in self._backward_branch_targets
                    and v < self.func_start
                    and self.X is not None):
                self._ensure_mutable_x(self._cur_x_type)
            if self.A is not None and v in self.valid_branch_targets:
                branch_a = self._materialize('A', self._cur_a_type)
            if self.X is not None and v in self.valid_branch_targets:
                branch_x = self._materialize('X', self._cur_x_type)
            if self.Y is not None and v in self.valid_branch_targets:
                branch_y = self._materialize('Y', self._cur_x_type)
            # Carry phi: if carry is a complex expression, save it to a variable
            # so that the merge label can assign the fall-through value on that path.
            # Needed for patterns like: CMP; BCS label; CMP2; label: BCS next
            # where the carry at label differs between branch-taken and fall-through.
            if (self.carry is not None and v in self.valid_branch_targets
                    and not self._simple(self.carry)):
                carry_var = self._alloc_tmp('uint8')
                self._emit(f'{carry_var} = ({self.carry}) ? 1 : 0;')
                self.carry = carry_var
            # If another branch already targeted this label with a different A,
            # we need a phi variable. Assign the current A to the first branch's
            # A_var so both paths converge on the same variable at the label.
            existing = self._branch_states.get(v)
            if existing and existing.get('A_var') and branch_a and existing['A_var'] != branch_a:
                # Merge: assign current A to the earlier branch's variable
                self._emit(f'{existing["A_var"]} = {self.A};')
                branch_a = existing['A_var']  # use the same merge var
            self._branch_states[v] = {
                'A_var': branch_a,
                'X_var': branch_x,
                'Y_var': branch_y,
                'carry': self.carry,
                'carry_var': carry_var,
                'stack': list(self.stack),
            }
            # For backward branches: sync A/X/Y with the label's variables.
            # The label was already emitted, so we can't merge there.
            # Instead, assign current A/X/Y to the label's A/X/Y before the goto.
            # Skip X sync if label's X was shared with Y (would corrupt loop index).
            if hasattr(self, '_label_a') and v in self._label_a:
                la = self._label_a[v]
                if la and self.A and la != self.A and self._simple(la):
                    self._emit(f'{la} = {self.A};')
            if hasattr(self, '_label_x') and v in self._label_x:
                lx = self._label_x[v]
                ly = self._label_y.get(v)
                if lx and self.X and lx != self.X and self._simple(lx) and self._simple(self.X):
                    if lx != ly:  # don't sync X into a var shared with Y
                        self._emit(f'{lx} = {self.X};')
                if ly and self.Y and ly != self.Y and self._simple(ly) and self._simple(self.Y):
                    if ly != lx:  # don't sync Y into a var shared with X
                        self._emit(f'{ly} = {self.Y};')
            # If branch target is past the function end (another function), emit
            # a conditional tail call instead of a goto.  But if the decoder
            # already decoded the target (it's in valid_branch_targets), the
            # emitter loop emits code there with a label — use goto, not a
            # tail call or return.
            if self.end_addr and v >= self.end_addr and v not in self.valid_branch_targets:
                if self._emit_tail_call(v, cond=cond):
                    pass  # tail call emitted
                else:
                    self._warn(f'{mn} ${v:04X} treated as return --outside decoded range',
                               f"Add 'end:{v:04X}' or 'name {(self.bank<<16)|v:06X} <Name>' to cfg")
                    rv = self._return_value_expr()
                    if rv is None:
                        self._emit(f'if ({cond}) return;')
                    else:
                        self._emit(f'if ({cond}) return {rv};')
            else:
                self._emit(f'if ({cond}) goto label_{v:04x};')

    def _emit_jmp(self, mode: int, v: int):
        if mode == ABS:
            # Check if target is past function end (cross-function jump)
            # Skip tail call if target was decoded (has a label in this function)
            if self.end_addr and v >= self.end_addr and v not in self.valid_branch_targets:
                if self._emit_tail_call(v):
                    return
                # Fall through to normal handling if tail call fails
            if v not in self.valid_branch_targets:
                if self._emit_tail_call(v):
                    return
                self._warn(f'JMP ${v:04X} treated as return --outside decoded range',
                           f"Add 'end:{v:04X}' or 'name {(self.bank<<16)|v:06X} <Name>' to cfg")
                self._emit_return_for_current_sig()
            else:
                self._emit(f'goto label_{v:04x};')
        elif mode == LONG:
            fname = self._callee(v)
            callee_sig = self.func_sigs.get(v)
            _ret, callee_params = parse_sig(callee_sig)
            call_args = self._build_call_args(callee_params)
            if _ret != 'void' and self.ret_type != 'void':
                self._emit(f'return {fname}({call_args});')
            else:
                self._emit(f'{fname}({call_args});')
                self._emit_return_for_current_sig()
        elif mode in (INDIR, INDIR_X):
            self._warn(f'JMP ({MODE_STR[mode]} ${v:04x}) dispatch --needs verbatim body',
                       "Add 'skip <FuncName>' and provide verbatim body in cfg")
            self._emit_return_for_current_sig()
        else:
            self._emit(f'/* JMP {MODE_STR.get(mode,"?")} ${v:x} */')

    def _emit_call(self, insn: Insn):
        """JSL / JSR --handles dispatch tables and return value propagation.

        HANDOFF requirement C: When a callee returns uint16, that means it
        modified X. Assign the return back to the EXISTING X variable.
        """
        mn = insn.mnem
        v = insn.operand
        # JSL and JMP LONG (JML) have 24-bit operands; JSR is bank-local.
        target = v if (mn == 'JSL' or insn.mode == LONG) else ((self.bank << 16) | v)

        # Inline dispatch table
        if insn.dispatch_entries:
            self._emit_dispatch(insn, target)
            return

        # Save pre-call X/Y for x_after/y_after tracking (before call args
        # are built and before registers get clobbered by return conventions).
        pre_call_x = self.X
        pre_call_y = self.Y

        fname = self._callee(target)
        callee_sig = self.func_sigs.get(target)
        _ret, callee_params = parse_sig(callee_sig)
        call_args = self._build_call_args(callee_params)

        if _ret != 'void':
            if _ret in _STRUCT_RETURN_DP:
                # Struct return: capture result, inject fields into dp_state so
                # subsequent DP reads (LDA $00 etc.) see the correct values.
                tmp = self._alloc(_ret)
                self._emit(f'{tmp} = {fname}({call_args});')
                for dp_addr, field, _ctype in _STRUCT_RETURN_DP[_ret]:
                    self.dp_state[dp_addr] = f'{tmp}.{field}'
                # A typically holds the first field after callee's last STA + RTS.
                first_field = _STRUCT_RETURN_DP[_ret][0][1]  # e.g. 'first'
                self.A = f'{tmp}.{first_field}'
            elif _ret == 'PairU16':
                # PairU16 returns via A (first) and X (second) registers.
                tmp = self._alloc(_ret)
                self._emit(f'{tmp} = {fname}({call_args});')
                a_tmp = self._alloc('uint8')
                self._emit(f'{a_tmp} = {tmp}.first;')
                self.A = a_tmp
                x_tmp = self._alloc('uint8')
                self._emit(f'{x_tmp} = {tmp}.second;')
                self.X = x_tmp
                self.flag_src = self.X
            elif _ret == 'RetAY':
                # RetAY returns via A (.a) and Y (.y) registers.
                tmp = self._alloc(_ret)
                self._emit(f'{tmp} = {fname}({call_args});')
                a_tmp = self._alloc('uint8')
                self._emit(f'{a_tmp} = {tmp}.a;')
                self.A = a_tmp
                y_tmp = self._alloc('uint8')
                self._emit(f'{y_tmp} = {tmp}.y;')
                self.Y = y_tmp
                self.flag_src = self.A
            elif _ret == 'RetY':
                # RetY returns via Y register only.
                tmp = self._alloc(_ret)
                self._emit(f'{tmp} = {fname}({call_args});')
                y_tmp = self._alloc('uint8')
                self._emit(f'{y_tmp} = {tmp}.y;')
                self.Y = y_tmp
                self.flag_src = self.Y
                # The callee may have scribbled on A as scratch; drop
                # the caller's A tracking so code that reads A after
                # the call doesn't use stale pre-call values. Callers
                # that consume A usually overwrite it explicitly (TYA,
                # LDA ...).
                self.A = None
            elif _ret == 'uint16' and self.X and self._simple(self.X):
                # HANDOFF requirement C: return value updates existing X.
                # A is NOT modified by the callee (65816 convention: uint16
                # returns are in X, A is preserved).
                self._emit(f'{self.X} = {fname}({call_args});')
                self.flag_src = self.X
            else:
                tmp = self._alloc(_ret)
                self._emit(f'{tmp} = {fname}({call_args});')
                self.A = tmp
                self.carry = tmp
                self.flag_src = tmp
                if _ret == 'uint16':
                    self.X = tmp
            # Preserve Y across calls — JSR/JSL do not clobber Y in real
            # 65816.  Callers that need Y = A use explicit TAY.  Callees
            # that modify Y are handled by y_after / RetAY / RetY /
            # restores_x / explicit-clobber-set.
            if _ret not in ('RetAY', 'RetY'):
                # If we've inferred that the callee writes Y without a
                # PHY/PLY save-restore, the ROM's Y after the call is
                # whatever the callee left behind, not our pre-call
                # value. Drop tracking so downstream TYA / LDA $xx,Y
                # emits a warning (honest) instead of silently using a
                # stale pre-call expression (wrong).
                clobbers = self.callee_clobbers.get(target, set())
                if 'Y' in clobbers:
                    self.Y = None
                else:
                    self.Y = pre_call_y
            # ReturnsTwice pattern: the callee manipulates the stack to skip
            # the caller's remaining code when the sprite is offscreen/invalid.
            # The emitter must inject the early return that the callee would
            # have triggered via stack manipulation on the real 65816.
            if 'ReturnsTwice' in fname or 'Recomp' in fname:
                rv = self.A
                if _ret == 'bool':
                    self._emit(f'if ({rv}) return;')
                elif _ret == 'uint8':
                    if self.ret_type == 'void':
                        self._emit(f'if ({rv} == 0xff) return;')
                    elif self.ret_type == 'uint8':
                        self._emit(f'if ({rv} == 0xff) return {rv};')
                    elif self.ret_type == 'RetY':
                        self._emit(f'if ({rv} == 0xff) return (RetY){{ .y = {rv} }};')
                    elif self.ret_type == 'RetAY':
                        self._emit(
                            f'if ({rv} == 0xff) return (RetAY){{ .a = {rv},'
                            f' .y = {rv} }};'
                        )
                    else:
                        # Other struct/complex outer types: emit the bare
                        # return expr (may still fail to compile if rv type
                        # doesn't match, but that's a separate sig issue).
                        self._emit(f'if ({rv} == 0xff) return {rv};')
        else:
            self._emit(f'{fname}({call_args});')
            # Void-return callee: no return value to track, but the callee
            # may still clobber A/X in the 65816 sense (writes without a
            # PHA/PHX save-restore). Drop any register we know the callee
            # clobbers so subsequent reads don't pretend the pre-call
            # expression is still valid.
            clobbers = self.callee_clobbers.get(target, set())
            if 'A' in clobbers:
                self.A = None
            if 'X' in clobbers and self.X not in ('k', 'j'):
                # X is often passed-through as the k param; only drop
                # tracking if the callee clobbered X AND we weren't
                # carrying the sprite-slot parameter through unchanged.
                self.X = None
            if 'Y' in clobbers:
                self.Y = None

        # Track DP output values from callee's pointer output params.
        # If the callee has a pointer param like PointU16_*pt_out, it writes
        # to known DP addresses. Create output variables and inject into dp_state
        # so subsequent calls can pick them up.
        has_ptr_output = False
        for _ptype, pname in callee_params:
            if not pname.startswith('*'):
                continue
            has_ptr_output = True
            base_type = _ptype.rstrip('*').strip()
            dp_fields = _STRUCT_OUTPUT_DP.get(base_type)
            if not dp_fields:
                continue
            for dp_lo, dp_hi, field, ctype in dp_fields:
                vname = self._alloc(ctype)
                self._emit(f'{vname} = PAIR16(g_ram[0x{dp_hi:02x}], g_ram[0x{dp_lo:02x}]);')
                # Store as 16-bit wide value in dp_state for the low address
                self.dp_state[dp_lo] = vname
        # HandleNormalSpriteLevelColl_019441/_01944D restore Y from DP $0F
        # before returning (STY $0F on entry, LDY $0F before RTS).
        # Override Y = retval with the actual restored value from g_ram.
        has_pointu16_out = any(
            _ptype.rstrip('*').strip() == 'PointU16' and pname.startswith('*')
            for _ptype, pname in callee_params)
        if has_pointu16_out:
            yname = self._alloc('uint8')
            self._emit(f'{yname} = g_ram[0x0f];')
            self.Y = yname
            self.dp_state[0x0f] = yname

        # restores_x: callee explicitly sets X to a known value before RTS.
        # Update our X tracking so subsequent TXA/LDA $xx,X use the correct value.
        x_restore = self.x_restores_map.get(target)
        if x_restore:
            self.X = x_restore

        # x_after: callee modifies X by a known increment (e.g. INX INX before RTS).
        # Use the PRE-CALL X (saved before self.X was potentially clobbered).
        x_inc = self.x_after_map.get(target)
        if x_inc and pre_call_x is not None:
            new_x = self._alloc('uint8')
            self._emit(f'{new_x} = (uint8)({pre_call_x} + {x_inc});')
            self.X = new_x

        # y_after: callee modifies Y by a known increment (e.g. INY INY before RTS).
        # Use the PRE-CALL Y (saved before self.Y was clobbered by return conventions).
        y_inc = self.y_after_map.get(target)
        if y_inc and pre_call_y is not None:
            new_y = self._alloc('uint8')
            self._emit(f'{new_y} = (uint8)({pre_call_y} + {y_inc});')
            self.Y = new_y

    def _emit_dispatch(self, insn: Insn, target: int):
        tbl_addr = (insn.addr + insn.length) & 0xFFFF

        # Per-entry dispatch emission:
        #   - Entry is a known named function (cross-function dispatch) → call + return.
        #   - Entry is an intra-function branch target (e.g. label decoded for this
        #     function) → goto label_XXXX.
        # If every entry is a known function we can compact to a function-pointer
        # table for the common case. Otherwise emit a per-case switch so mixed
        # external-call / internal-goto dispatches (the common ROM pattern for
        # sprite-status jump tables) work correctly.
        known_funcs = self.func_names  # {(bank<<16)|addr: name}

        def _entry_is_known_func(e: int) -> bool:
            return ((self.bank << 16) | e) in known_funcs

        # Null entries (0) are "no handler" sparse-table slots. Accept them
        # for the compact form (emitted as NULL, guarded at dispatch), and
        # emit `case i: return;` in switch form.
        all_external_named = all(e == 0 or _entry_is_known_func(e)
                                 for e in insn.dispatch_entries) and \
                             any(e != 0 for e in insn.dispatch_entries)

        if self.A is None:
            self._warn('A unknown at dispatch --defaulting index to 0')
            idx = '0'
        else:
            idx = self._materialize('A')

        if all_external_named:
            # Compact form: function pointer table. Null entries become
            # NULL and are skipped at dispatch time.
            arr_name = f'kDispatch_{tbl_addr:04x}'
            func_type = 'FuncU8' if self.has_k else 'FuncV'
            call_arg = 'k' if self.has_k else ''
            has_null = any(e == 0 for e in insn.dispatch_entries)
            # Cast each handler to the dispatch table type. Some handlers
            # declare param types like `const uint8 *p0` or struct pointers
            # whose register-passing convention the SNES dispatch doesn't
            # honor — the ROM sets DP/registers before the JSR and doesn't
            # pass C-level args through the table. Casting silences C4113
            # (param-list mismatch) without changing runtime behavior since
            # the handlers read their inputs from WRAM/DP, not from the
            # function's nominal C parameters.
            self._emit(f'{{ static {func_type} *const {arr_name}[] = {{')
            for entry in insn.dispatch_entries:
                if entry == 0:
                    self._emit(f'  (void*)0,  /* null dispatch */')
                else:
                    fn = self._callee((self.bank << 16) | entry)
                    self._emit(f'  ({func_type}*)&{fn},')
            self._emit('};')
            if has_null:
                self._emit(f'if ({arr_name}[{idx}]) {arr_name}[{idx}]({call_arg}); }}')
            else:
                self._emit(f'{arr_name}[{idx}]({call_arg}); }}')
            # Dispatch is terminal: emit the outer function's return to
            # satisfy the declared ret type. Handlers in the table are
            # typically void, so for a uint8/RetAY outer we fall back to
            # _return_value_expr's default (A if tracked, else 0).
            self._emit_return_for_current_sig()
            return

        # Mixed / unknown entries: per-case switch. Each case is terminal,
        # so its return must match the outer function's declared ret type.
        # Per-callee arg list: switch cases emit direct function calls
        # (not through a FuncU8 cast), so each call must match the
        # callee's declared sig. A handler declared `(uint8 k)` needs
        # an actual arg — use _build_call_args to pull the current X
        # track (or a RECOMP_WARN fallback if X is unknown).
        rv = self._return_value_expr()
        ret_stmt = 'return;' if rv is None else f'return {rv};'
        self._emit(f'switch ({idx}) {{')
        for i, entry in enumerate(insn.dispatch_entries):
            if entry == 0:
                # Null dispatch slot: unused object ID in a sparse table.
                self._emit(f'  case {i}: {ret_stmt}  /* null dispatch */')
            elif _entry_is_known_func(entry):
                fn = self._callee((self.bank << 16) | entry)
                callee_sig = (self.func_sigs or {}).get((self.bank << 16) | entry)
                _cr, callee_params = parse_sig(callee_sig) if callee_sig else ('void', [])
                call_args = self._build_call_args(callee_params)
                self._emit(f'  case {i}: {fn}({call_args}); {ret_stmt}')
            else:
                # Unknown — assume an intra-function branch target. If the
                # label does not exist at link time the C compiler will error,
                # surfacing the missing cfg name.
                self._emit(f'  case {i}: goto label_{entry:04x};')
        self._emit(f'}}')
        # Fallthrough past the switch would have no return statement for
        # non-void sigs. Emit one matching the ret type.
        if rv is not None:
            self._emit(ret_stmt)


# ==============================================================================
# FUNCTION EMISSION (top-level)
# ==============================================================================

def emit_function(name: str, insns: List[Insn], bank: int,
                  func_names: Dict[int, str],
                  func_sigs: Dict[int, str] = None,
                  sig: Optional[str] = None,
                  trace: bool = False,
                  next_func: Optional[Tuple[str, Optional[str]]] = None,
                  hints: Dict[str, str] = None,
                  dp_sync: Dict[int, str] = None,
                  rom: bytes = None,
                  x_restores_map: Dict[int, str] = None,
                  y_after_map: Dict[int, int] = None,
                  x_after_map: Dict[int, int] = None,
                  callee_clobbers: Dict[int, Set[str]] = None,
                  end_addr: int = 0,
                  decl_ret_override: Optional[str] = None) -> List[str]:
    """Emit a complete C function from decoded instructions.
    next_func: (name, sig) of the function immediately following in ROM, for fall-through.
    hints: dict of cfg hints like {'init_y': 'x'} to initialize Y from X on entry.
    dp_sync: ORACLE BRIDGE --{dp_addr: sync_func} to call after writing to dp_addr.
    decl_ret_override: override the C return type in the DEFINITION header line
        without changing the body's internal return tracking. Used only for
        pointer-return functions where cfg says void but funcs.h declares
        a pointer — the body stays void (avoids A-tracking confusion) while
        the declared type keeps oracle/hand-written callers consistent.
    """
    hints = hints or {}
    ret_type, params = parse_sig(sig)
    param_str = format_param_str(params)

    init_x, init_a, init_b, init_carry = None, None, None, None
    for _ptype, pname in params:
        if pname == 'k' and init_x is None:
            # k (sprite slot) -> X register
            init_x = pname
        elif pname == 'j' and init_x is None and not any(pn == 'k' for _, pn in params):
            # j-only functions: j is passed via Y register, not X.
            # Do NOT set init_x; Y will be initialized below.
            pass
        elif pname == 'a':
            init_a = pname
            if _ptype == 'uint16':
                # uint16 a: lo byte in A, hi byte in B (via XBA idiom).
                # Initialize B = HIBYTE(a) so that after XBA, A = HIBYTE(a).
                init_b = 'HIBYTE(a)'
        elif pname == 'cr':
            # cr = carry flag input --initialize carry register to 'cr'.
            init_carry = 'cr'

    # Hint-based init_carry overrides param-based init_carry.
    if 'init_carry' in hints:
        init_carry = hints['init_carry']

    c_ret_type = decl_ret_override or ret_type

    if not insns:
        return [f'{c_ret_type} {name}({param_str}) {{}}']

    start = insns[0].addr
    start16 = start & 0xFFFF

    # Collect branch targets
    decoded_addrs = {insn.addr & 0xFFFF for insn in insns}
    branch_targets: Set[int] = set()
    for insn in insns:
        if insn.mnem in ('BPL','BMI','BEQ','BNE','BCC','BCS','BVS','BVC','BRA','BRL'):
            tgt = insn.operand
            if tgt >= start16 or tgt in decoded_addrs:
                branch_targets.add(tgt)
        elif insn.mnem == 'JMP' and insn.mode == ABS:
            tgt = insn.operand
            if tgt >= start16 or tgt in decoded_addrs:
                branch_targets.add(tgt)
        # Include dispatch table entries as branch targets
        if insn.dispatch_entries:
            for entry in insn.dispatch_entries:
                if entry in decoded_addrs:
                    branch_targets.add(entry)

    valid_branch_targets = branch_targets & decoded_addrs

    # Backward branch targets = loop headers.
    # Phase 1: explicit backward branches (target < branch address).
    backward_branch_targets: Set[int] = set()
    for insn in insns:
        if insn.mnem in ('BPL','BMI','BEQ','BNE','BCC','BCS','BVS','BVC','BRA','BRL','JMP'):
            tgt = insn.operand
            if tgt < (insn.addr & 0xFFFF) and tgt in valid_branch_targets:
                backward_branch_targets.add(tgt)

    # Phase 2: detect implicit backward loops via fall-through + branch cycles.
    # Build a mini-CFG: for each instruction, what addresses can follow it?
    # Then find addresses reachable from a backward branch target that can
    # reach back to that target (forming a cycle that includes fall-through).
    insn_by_addr = {insn.addr & 0xFFFF: insn for insn in insns}
    sorted_addrs = sorted(insn_by_addr.keys())
    addr_to_idx = {a: i for i, a in enumerate(sorted_addrs)}

    def _successors(addr):
        """Return set of possible successor addresses for instruction at addr."""
        insn = insn_by_addr.get(addr)
        if insn is None:
            return set()
        mn = insn.mnem
        succs = set()
        # Unconditional transfers
        if mn in ('RTS', 'RTL', 'RTI'):
            return set()  # no successors
        if mn in ('JMP', 'BRA', 'BRL'):
            if insn.mode == ABS and insn.operand in insn_by_addr:
                succs.add(insn.operand)
            return succs
        # Conditional branches: both taken and fall-through
        if mn in ('BPL','BMI','BEQ','BNE','BCC','BCS','BVS','BVC'):
            tgt = insn.operand
            if tgt in insn_by_addr:
                succs.add(tgt)
            # fall-through
            idx = addr_to_idx.get(addr)
            if idx is not None and idx + 1 < len(sorted_addrs):
                succs.add(sorted_addrs[idx + 1])
            return succs
        # All other instructions: fall through
        idx = addr_to_idx.get(addr)
        if idx is not None and idx + 1 < len(sorted_addrs):
            succs.add(sorted_addrs[idx + 1])
        return succs

    # For each existing backward branch target, find all addresses reachable
    # from it. Then check if any of those addresses can reach back to the
    # target via fall-through or branches — if so, the ENTRY point of that
    # cycle is also a backward branch target.
    # More general: find ALL addresses that are part of a cycle.
    # Use iterative reachability: from each branch target, BFS forward;
    # if we reach the target again, it's confirmed as a loop header.
    # Also check: any valid_branch_target that is reachable from an address
    # AFTER it (via the successor graph) is a potential loop header.
    # Phase 2: detect implicit backward loops via fall-through after inner loops.
    # Pattern: a conditional backward branch at addr B falls through to addr F.
    # F is a valid branch target (reached by JMP/BRA from code before B).
    # If there's a forward branch from within [F, B] that targets an address
    # in the backward branch's inner loop range [T, B] (where T is the backward
    # target), then F is an outer loop header — control flows:
    # F → ... → forward_branch to inner → inner loop → B → fall-through to F.
    # Outer loop headers: labels that need a goto to close an implicit outer loop
    # (fall-through after inner loop back to outer loop header). These are tracked
    # SEPARATELY from backward_branch_targets because backward_branch_targets
    # triggers dp_state clear + WatchdogCheck + register snapshotting at the label,
    # which changes code generation on the linear path. Outer loop headers only
    # need the goto — the dp_state clear already happens at all branch targets.
    outer_loop_headers: Set[int] = set()
    backward_branch_insns = []
    for insn in insns:
        if insn.mnem in ('BPL','BMI','BEQ','BNE','BCC','BCS','BVS','BVC'):
            tgt = insn.operand
            iaddr = insn.addr & 0xFFFF
            if tgt < iaddr and tgt in valid_branch_targets:
                backward_branch_insns.append((iaddr, tgt))
    for bb_addr, bb_tgt in backward_branch_insns:
        bb_idx = addr_to_idx.get(bb_addr)
        if bb_idx is None or bb_idx + 1 >= len(sorted_addrs):
            continue
        ft_addr = sorted_addrs[bb_idx + 1]
        if ft_addr in backward_branch_targets:
            continue  # already a real backward target
        if ft_addr not in valid_branch_targets:
            continue
        found = False
        for insn_check in insns:
            ca = insn_check.addr & 0xFFFF
            if ca < ft_addr:
                continue
            if insn_check.mnem in ('BPL','BMI','BEQ','BNE','BCC','BCS','BVS','BVC','JMP','BRA','BRL'):
                ct = insn_check.operand
                if bb_tgt <= ct <= bb_addr:
                    found = True
                    break
        if found:
            outer_loop_headers.add(ft_addr)

    ctx = EmitCtx(bank, func_names, func_sigs=func_sigs,
                  init_x=init_x, init_a=init_a, init_b=init_b, init_carry=init_carry,
                  ret_type=ret_type,
                  func_start=start, valid_branch_targets=valid_branch_targets,
                  backward_branch_targets=backward_branch_targets,
                  dp_sync=dp_sync, rom=rom,
                  carry_ret=hints.get('carry_ret', '0') != '0',
                  x_restores_map=x_restores_map,
                  y_after_map=y_after_map,
                  x_after_map=x_after_map,
                  callee_clobbers=callee_clobbers)
    ctx._ret_y = hints.get('ret_y', '0') != '0'
    ctx.end_addr = end_addr

    # Seed dp_state with DP-mapped parameters so that LDA $XX uses the
    # parameter name instead of g_ram[0xXX].
    for _ptype, pname in params:
        dp_addr = _param_to_dp(pname)
        if dp_addr is not None:
            ctx.dp_state[dp_addr] = pname

    # Initialize Y register.
    # 65816 convention: when a function has both k and j params, k=X and j=Y.
    # When only k, use init_y hint (typically Y=X=k from default_init_y).
    has_j_param = any(pn == 'j' for _pt, pn in params)
    has_k_param = any(pn == 'k' for _pt, pn in params)
    if has_j_param:
        # j is always passed via Y register (whether or not k is also present).
        ctx.Y = 'j'
    elif 'init_y' in hints:
        iy = hints['init_y']
        if iy == 'x' and ctx.X is not None:
            ctx.Y = ctx.X
        elif iy == 'a' and ctx.A is not None:
            ctx.Y = ctx.A
        else:
            ctx.Y = iy

    # Emit instructions. The decoder may include instructions past end_addr
    # for branch target resolution. For past-end instructions:
    # - If it's a branch target from within the function: emit normally
    #   (the function branches there and needs the code to execute)
    # - Otherwise: emit label only, skip body (prevents register corruption
    #   from the next function's entry instruction affecting fall-through)
    _in_func_branch_targets = set()
    for insn in insns:
        pc16 = insn.addr & 0xFFFF
        if pc16 < (end_addr or 0xFFFF):
            if insn.mnem in ('BPL','BMI','BEQ','BNE','BCC','BCS','BVS','BVC','BRA','BRL','JMP'):
                if insn.mode == ABS or insn.mnem not in ('JMP',):
                    _in_func_branch_targets.add(insn.operand)

    # Compute the full set of past-end addresses that need code emitted.
    # Start from direct branch targets, then flood-fill fall-through until
    # a terminator (RTS/RTL/RTI/JMP/BRA/BRL) is reached.  This ensures that
    # branches into past-end code execute the full code path, not just the
    # single instruction at the branch target.
    _past_end_emit = set()
    if end_addr:
        _terminators = ('RTS', 'RTL', 'RTI')
        _uncond = ('BRA', 'BRL')
        # Build addr→insn map for past-end instructions
        _past_end_insns = {}
        _past_end_order = []
        for insn in insns:
            pc16 = insn.addr & 0xFFFF
            if pc16 >= end_addr:
                _past_end_insns[pc16] = insn
                _past_end_order.append(pc16)
        # Seed from branch targets and flood-fill
        _worklist = [a for a in _past_end_order if a in _in_func_branch_targets]
        _visited = set()
        while _worklist:
            addr = _worklist.pop(0)
            if addr in _visited:
                continue
            _visited.add(addr)
            if addr not in _past_end_insns:
                continue
            _past_end_emit.add(addr)
            ins = _past_end_insns[addr]
            is_term = ins.mnem in _terminators
            is_uncond_jmp = (ins.mnem == 'JMP' and ins.mode in (ABS, LONG, INDIR, INDIR_X))
            is_uncond_br = ins.mnem in _uncond
            if is_term or is_uncond_jmp or is_uncond_br:
                continue  # don't follow fall-through past terminators
            # Follow fall-through to next instruction
            next_addr = addr + ins.length
            if next_addr in _past_end_insns:
                _worklist.append(next_addr)
            # Also follow conditional branch targets within past-end region
            if ins.mnem in ('BPL','BMI','BEQ','BNE','BCC','BCS','BVS','BVC'):
                tgt = ins.operand
                if tgt in _past_end_insns:
                    _worklist.append(tgt)

    # last_before_end must be the HIGHEST-pc instruction in [start, end),
    # not the last-emitted in decode order. Decode order is unreliable when the
    # decoder chases backward-branch targets past-start (e.g. a BPL into the
    # previous function's RTS); the chased RTS gets decoded LAST but sits at
    # a lower address than the actual end-of-function instruction. Using
    # decode order causes is_terminal to look at the wrong insn and
    # incorrectly suppress fall-through emit (see GameModeXX_FadeInOrOut).
    #
    # Also: past-start insns (reached via backward branches into the previous
    # function's tail, e.g. BPL into an RTS) must be emitted AFTER the
    # fall-through call, not in decode order. Emitting them inline would
    # place a `return` before the fall-through, making the fall-through
    # unreachable. We defer them to _past_start_buf and flush after the
    # fall-through emit below.
    last_before_end = insns[-1]
    _last_pc = -1
    _past_start_lines = None
    _past_start_buf: List = []  # (insn,) records to emit after fall-through
    for insn in insns:
        pc16 = insn.addr & 0xFFFF
        if end_addr and pc16 >= end_addr:
            if pc16 in _past_end_emit:
                # This past-end address is reachable from within the function.
                # Emit it normally so the branch target has real code.
                ctx.emit(insn, valid_branch_targets)
            else:
                # Not reachable — emit label only, skip body.
                if pc16 in valid_branch_targets:
                    ctx._emit(f'label_{pc16:04x}:;')
            continue
        if start16 and pc16 < start16:
            # Defer past-start insn to be emitted after the fall-through tail.
            _past_start_buf.append(insn)
            continue
        if pc16 > _last_pc:
            _last_pc = pc16
            last_before_end = insn
        ctx.emit(insn, valid_branch_targets)

        # After a conditional branch whose TARGET is a backward branch
        # (inner loop back-edge), check if the fall-through address is ALSO
        # a backward branch target (outer loop header). If so, emit an
        # explicit goto to close the outer loop.
        # This pattern: BNE inner_loop; <fall-through to outer_loop_header>
        # The C fall-through just continues to the next statement, but the
        # 65816 fall-through to the outer loop header means "iterate again."
        if insn.mnem in ('BPL','BMI','BEQ','BNE','BCC','BCS','BVS','BVC'):
            branch_tgt = insn.operand
            # Only if the branch target is backward (inner loop back-edge)
            if branch_tgt < pc16 and branch_tgt in valid_branch_targets:
                cur_idx = addr_to_idx.get(pc16)
                if cur_idx is not None and cur_idx + 1 < len(sorted_addrs):
                    next_addr = sorted_addrs[cur_idx + 1]
                    if (next_addr in outer_loop_headers
                            and not ctx._is_always_taken(insn.mnem)):
                        ctx._emit(f'goto label_{next_addr:04x};  /* outer loop */')

    # Fall-through detection: use the last instruction BEFORE end_addr.
    # Also check if the skipped boundary instruction was terminal (the function's
    # actual last instruction might be an RTS/JMP at end_addr that the decoder
    # placed at the next function's start for branch resolution).
    last = last_before_end
    boundary_insn = next((i for i in insns if end_addr and (i.addr & 0xFFFF) == end_addr), None)
    is_terminal = (last.mnem in ('RTL', 'RTS', 'RTI', 'JMP', 'BRA', 'BRL')
                   or getattr(last, 'dispatch_terminal', False))
    if boundary_insn and boundary_insn.mnem in ('RTL', 'RTS', 'RTI', 'JMP', 'BRA', 'BRL'):
        is_terminal = True  # the boundary instruction was the real terminator
    if not is_terminal and next_func:
        nf_name, nf_sig = next_func
        nf_ret, nf_params = parse_sig(nf_sig)
        # Build args from current register state
        nf_args = ctx._build_call_args(nf_params)
        # Fall-through to next_func IS semantically a tail call: our return
        # address becomes next_func's return address. Pop our own recomp
        # stack frame BEFORE calling so the frame accounting matches the
        # native stack shape (caller expects exactly one level of depth for
        # us, not two). Matches _emit_tail_call / RTS/RTL handlers.
        if ret_type in _STRUCT_RETURN_DP:
            # Struct return via fall-through: call next func for its side effects
            # (it writes the struct fields to WRAM via its own STAs), then build
            # the struct from g_ram. Clear dp_state for struct fields so the g_ram
            # fallback is used (dp_state may hold stale 8-bit values from this func).
            # Pop must happen after the side-effect call so the callee still sees
            # our frame during its own push/pop, then we reconstruct + return.
            ctx._emit(f'{nf_name}({nf_args});  /* fall-through */')
            for dp_addr, _field, _ctype in _STRUCT_RETURN_DP[ret_type]:
                ctx.dp_state.pop(dp_addr, None)
            ctx._emit(f'RecompStackPop();')
            ctx._emit(f'return {ctx._struct_ret_expr(ret_type)};')
        elif nf_ret != 'void' and ret_type != 'void':
            ctx._emit(f'RecompStackPop();')
            call_expr = f'{nf_name}({nf_args})'
            if ret_type == nf_ret:
                ctx._emit(f'return {call_expr};  /* fall-through */')
            elif ret_type == 'RetY' and nf_ret in ('uint8', 'uint16'):
                ctx._emit(
                    f'return (RetY){{ .y = {call_expr} }};  /* fall-through */'
                )
            elif ret_type == 'RetAY' and nf_ret in ('uint8', 'uint16'):
                ctx._emit(
                    f'return (RetAY){{ .a = {call_expr}, .y = 0 }};'
                    '  /* fall-through */'
                )
            elif ret_type in ('uint8', 'uint16') and nf_ret == 'RetAY':
                # Scalar outer consuming a RetAY tail call — extract .a.
                tmp = ctx._alloc('RetAY')
                ctx._emit(f'{tmp} = {call_expr};  /* fall-through */')
                ctx._emit(f'return {tmp}.a;')
            elif ret_type in ('uint8', 'uint16') and nf_ret == 'RetY':
                # Scalar outer consuming a RetY tail call — extract .y.
                tmp = ctx._alloc('RetY')
                ctx._emit(f'{tmp} = {call_expr};  /* fall-through */')
                ctx._emit(f'return {tmp}.y;')
            elif ret_type == 'RetAY' and nf_ret == 'RetY':
                tmp = ctx._alloc('RetY')
                ctx._emit(f'{tmp} = {call_expr};  /* fall-through */')
                ctx._emit(f'return (RetAY){{ .a = 0, .y = {tmp}.y }};')
            elif ret_type == 'RetY' and nf_ret == 'RetAY':
                tmp = ctx._alloc('RetAY')
                ctx._emit(f'{tmp} = {call_expr};  /* fall-through */')
                ctx._emit(f'return (RetY){{ .y = {tmp}.y }};')
            else:
                ctx._emit(f'return {call_expr};  /* fall-through */')
        else:
            ctx._emit(f'RecompStackPop();')
            ctx._emit(f'{nf_name}({nf_args});  /* fall-through */')
            ctx._emit_return_for_current_sig()

    # Emit deferred past-start insns AFTER the fall-through emit. These are
    # reachable only via backward `goto label_XXXX` (e.g. BPL into the previous
    # function's RTS). Placing them after the fall-through keeps the normal
    # execution path intact while still resolving the goto target.
    for insn in _past_start_buf:
        ctx.emit(insn, valid_branch_targets)

    # Build output with hoisted declarations (HANDOFF requirement D)
    # Add stack-relative variables to hoisted declarations
    for sv in ctx._stk_vars:
        ctx._hoisted[sv] = 'uint16'

    lines = [f'{c_ret_type} {name}({param_str}) {{  // {start:06x}']

    if ctx._hoisted:
        by_type: Dict[str, List[str]] = {}
        for vname, typ in ctx._hoisted.items():
            by_type.setdefault(typ, []).append(vname)
        _primitive = {'uint8','uint16','uint32','int8','int16','int32','int','bool'}
        for typ, names in sorted(by_type.items()):
            zero = '0' if typ in _primitive else '{0}'
            inits = ', '.join(f'{n} = {zero}' for n in names)
            lines.append(f'  {typ} {inits};')

    # Push function name onto recomp call stack for watchdog diagnostics
    lines.append(f'  extern const char *g_last_recomp_func;')
    lines.append(f'  g_last_recomp_func = "{name}";')
    lines.append(f'  RecompStackPush("{name}");')
    if trace:
        lines.append(f'  extern void WatchdogCheck(void);')
        lines.append(f'  WatchdogCheck();')

    lines.extend(ctx.lines)
    lines.append('}')
    return lines


# ==============================================================================
# CONFIG FILE PARSER
# ==============================================================================

class Config:
    def __init__(self):
        self.bank: int = 0
        # funcs: [(name, start_addr, sig, end_override, mode_overrides, hints)]
        self.funcs: List[Tuple[str, int, Optional[str], Optional[int], Dict[int,int], Dict[str,str]]] = []
        self.names: Dict[int, str] = {}
        self.sigs: Dict[int, str] = {}
        self.x_restores: Dict[int, str] = {}  # {full_addr: expr} --after JSR to this addr, X = expr
        self.y_after: Dict[int, int] = {}  # {full_addr: increment} --after call, Y += increment
        self.x_after: Dict[int, int] = {}  # {full_addr: increment} --after call, X += increment
        self.skip: Set[str] = set()
        self.dispatch: Set[str] = set()  # functions provided by dispatch file (skip + no oracle fallback)
        self.data: List[dict] = []
        self.exclude_ranges: List[Tuple[int, int]] = []  # [(start, end)] --data ranges, don't decode
        self.no_autodiscover: Set[int] = set()  # local addrs blocked from intra-bank auto-promote
        self.includes: List[str] = []
        self.comment: str = ''
        self.verbatim: List[str] = []
        self.jsl_dispatch: Set[int] = set()
        self.jsl_dispatch_long: Set[int] = set()  # 3-byte long-pointer tables (A-indexed)
        self.default_init_y: Optional[str] = None  # bank-wide Y init hint
        # dp_sync: {dp_addr: sync_func_name} --call sync_func after writing to dp_addr
        self.dp_sync: Dict[int, str] = {}
        self._skip_all: bool = False  # skip all funcs (emit no bodies in gen)
        # preserves: {full_addr: set of registers ('A','X','Y') the callee
        # preserves}. Used for HLE/WRAM-resident callees whose body isn't
        # in ROM, so per-bank augmentation can't analyze them. The
        # liveness pass consumes this through cfg.clobbers (populated as
        # {A,X,Y} - preserves_set at import time).
        self.preserves: Dict[int, Set[str]] = {}


def parse_config(path: str) -> Config:
    cfg = Config()
    verbatim_active = False
    with open(path) as f:
        for raw in f:
            line = raw.rstrip()
            stripped = line.strip()

            if stripped == 'verbatim_start':
                verbatim_active = True; continue
            if stripped == 'verbatim_end':
                verbatim_active = False; continue
            if verbatim_active:
                cfg.verbatim.append(line); continue

            if not stripped or stripped.startswith('#'):
                continue

            # Strip inline comments (`# ...`) before tokenizing. Otherwise a
            # comment like `# oracle sig: uint16 loop counter` produces a
            # second `sig:` token that silently overwrites the real one
            # (seen on InitializeLevelData_Lo / _Hi, causing the emitted
            # definition to drop params and return unset).
            hash_idx = stripped.find(' #')
            if hash_idx >= 0:
                stripped = stripped[:hash_idx].rstrip()
            if not stripped:
                continue
            stripped = stripped.replace(' = ', ' ').replace('= ', ' ').replace(' =', ' ')
            parts = stripped.split()
            key = parts[0]

            if key == 'bank':
                cfg.bank = int(parts[1], 16)
            elif key == 'func':
                fname = parts[1]
                addr = int(parts[2], 16)
                sig = None
                end_override = None
                mode_overrides: Dict[int, int] = {}
                hints: Dict[str, str] = {}
                for tok in parts[3:]:
                    if tok.startswith('sig:'):
                        sig = tok[4:]
                    elif tok.startswith('end:'):
                        end_override = int(tok[4:], 16)
                    elif tok.startswith('rep:'):
                        addr_key = int(tok[4:], 16)
                        mode_overrides[addr_key] = mode_overrides.get(addr_key, 0) | 0x20
                    elif tok.startswith('repx:'):
                        addr_key = int(tok[5:], 16)
                        mode_overrides[addr_key] = mode_overrides.get(addr_key, 0) | 0x10
                    elif tok.startswith('sep:'):
                        mode_overrides[int(tok[4:], 16)] = 0x40  # SEP marker: force M=1 X=1
                    elif tok.startswith('init_y:'):
                        hints['init_y'] = tok[7:]  # e.g. 'x', 'k', 'j'
                    elif tok == 'carry_ret':
                        hints['carry_ret'] = '1'
                    elif tok == 'ret_y':
                        hints['ret_y'] = '1'
                    elif tok.startswith('init_carry:'):
                        hints['init_carry'] = tok[11:]
                    elif tok.startswith('restores_x:'):
                        hints['restores_x'] = tok[11:]
                    elif tok.startswith('y_after:'):
                        hints['y_after'] = tok[8:]  # e.g. '+2'
                    elif tok.startswith('x_after:'):
                        hints['x_after'] = tok[8:]  # e.g. '+2'
                cfg.funcs.append((fname, addr, sig, end_override, mode_overrides, hints))
                full = (cfg.bank << 16) | addr
                cfg.names[full] = fname
                if sig:
                    cfg.sigs[full] = sig
                if 'restores_x' in hints:
                    cfg.x_restores[full] = hints['restores_x']
                if 'y_after' in hints:
                    cfg.y_after[full] = int(hints['y_after'])
                if 'x_after' in hints:
                    cfg.x_after[full] = int(hints['x_after'])
            elif key == 'name':
                addr = int(parts[1], 16)
                cfg.names[addr] = parts[2]
                for tok in parts[3:]:
                    if tok.startswith('sig:'):
                        cfg.sigs[addr] = tok[4:]
                    elif tok.startswith('y_after:'):
                        cfg.y_after[addr] = int(tok[8:])
                    elif tok.startswith('x_after:'):
                        cfg.x_after[addr] = int(tok[8:])
            elif key == 'data':
                cfg.data.append({
                    'type': parts[1],
                    'decl': parts[2],
                    'addr': int(parts[3], 16),
                    'count': int(parts[4]) if len(parts) > 4 else 1,
                })
            elif key == 'skip':
                cfg.skip.add(parts[1])
            elif key == 'dispatch':
                cfg.skip.add(parts[1])      # don't auto-generate
                cfg.dispatch.add(parts[1])   # don't oracle-fallback either
            elif key == 'jsl_dispatch':
                cfg.jsl_dispatch.add(int(parts[1], 16))
            elif key == 'jsl_dispatch_long':
                cfg.jsl_dispatch_long.add(int(parts[1], 16))
            elif key == 'includes':
                cfg.includes.extend(parts[1:])
            elif key == 'default_init_y':
                cfg.default_init_y = parts[1]
            elif key == 'dp_sync':
                # dp_sync <addr_hex> <sync_func_name>
                # ORACLE BRIDGE: remove when decoupled from oracle
                dp_addr = int(parts[1], 16)
                cfg.dp_sync[dp_addr] = parts[2]
            elif key == 'comment':
                cfg.comment = ' '.join(parts[1:])
            elif key == 'exclude_range':
                # exclude_range <start_hex> <end_hex> --data range, don't decode
                er_start = int(parts[1], 16)
                er_end = int(parts[2], 16)
                cfg.exclude_ranges.append((er_start, er_end))
            elif key == 'no_autodiscover':
                # no_autodiscover <addr_hex> --block intra-bank auto-promote for this addr
                cfg.no_autodiscover.add(int(parts[1], 16))
            elif key == 'preserves':
                # preserves <full_addr_hex> [A] [X] [Y] --HLE/WRAM callee
                # preserves the listed registers. Empty list means preserves
                # none (callee clobbers everything -- same as default). Use
                # for callees not declared as `func` in any bank cfg, where
                # the recompiler can't infer clobbers from ROM code.
                full_addr = int(parts[1], 16)
                regs = set()
                for tok in parts[2:]:
                    if tok.upper() in ('A', 'X', 'Y'):
                        regs.add(tok.upper())
                cfg.preserves[full_addr] = regs
            elif key == 'skip_all':
                cfg._skip_all = True
            elif key == 'skip_all_except':
                cfg._skip_all = True
                cfg._skip_all_except = set(parts[1:])

    # skip_all: mark every func as skipped (no body emitted in gen)
    # skip_all_except: skip all except the listed functions
    if getattr(cfg, '_skip_all', False):
        except_set = getattr(cfg, '_skip_all_except', set())
        for fname, _a, _s, _e, _m, _h in cfg.funcs:
            if fname not in except_set:
                cfg.skip.add(fname)

    return cfg

# ==============================================================================
# ROM DATA EXTRACTION
# ==============================================================================

def extract_data_array(rom: bytes, bank: int, addr: int,
                       type_: str, decl: str, count: int) -> str:
    is_u16 = 'uint16' in type_
    byte_count = count * (2 if is_u16 else 1)
    raw = rom_slice(rom, bank, addr, byte_count)

    if is_u16:
        vals = [f'0x{raw[i] | (raw[i+1] << 8):x}' for i in range(0, byte_count, 2)]
    else:
        vals = [f'0x{b:x}' for b in raw]

    items = ', '.join(vals)
    return f'const {type_} {decl} = {{ {items}, }};'

# ==============================================================================
# HEXDUMP / DISASM
# ==============================================================================

def hexdump(rom: bytes, bank: int, addr: int, length: int):
    raw = rom_slice(rom, bank, addr, length)
    for i in range(0, len(raw), 16):
        chunk = raw[i:i+16]
        hex_ = ' '.join(f'{b:02x}' for b in chunk)
        asc = ''.join(chr(b) if 32 <= b < 127 else '.' for b in chunk)
        print(f'  {bank:02x}:{addr+i:04x}  {hex_:<47}  |{asc}|')

def disasm(rom: bytes, bank: int, start: int, end: int):
    insns = decode_func(rom, bank, start, end)
    for insn in insns:
        print(f'  {insn}')

# ==============================================================================
# FUNCS.H PARSER
# ==============================================================================

def parse_funcs_h(path: str) -> Dict[str, str]:
    """Parse funcs.h -> {func_name: sig_str}."""
    sigs: Dict[str, str] = {}
    decl_re = re.compile(r'^(\w[\w\s\*]*?)\s+(\w+)\s*\(([^)]*)\)\s*;')
    with open(path) as fh:
        for line in fh:
            line = line.strip()
            m = decl_re.match(line)
            if not m:
                continue
            ret_raw, fname, params_raw = m.group(1).strip(), m.group(2), m.group(3).strip()
            ret = ret_raw.replace(' ', '_') if ' ' in ret_raw else ret_raw
            if not params_raw or params_raw == 'void':
                sig = f'{ret}()'
            else:
                param_toks = []
                for param in params_raw.split(','):
                    param = param.strip()
                    if not param: continue
                    if '*' in param:
                        # Pointer param: "CollInfo *ci" -> "CollInfo_*ci"
                        # For plain data ptrs: "const uint8 *r6" -> "*r6"
                        star_idx = param.index('*')
                        pname = param[star_idx:].replace(' ', '')  # "*ci"
                        # Extract base type (strip const/volatile, take last word before *)
                        type_part = param[:star_idx].strip()
                        type_words = type_part.split()
                        # Filter out qualifiers
                        base_type = [w for w in type_words if w not in ('const', 'volatile', 'struct')]
                        if base_type and base_type[-1] not in ('uint8', 'uint16', 'int8', 'int16', 'char', 'void'):
                            # Struct pointer -- preserve type: "CollInfo_*ci"
                            param_toks.append(f'{base_type[-1]}_{pname}')
                        else:
                            param_toks.append(pname)
                    else:
                        param_toks.append('_'.join(param.split()))
                sig = f'{ret}({",".join(param_toks)})'
            sigs[fname] = sig
    return sigs

# ==============================================================================
# MAIN PIPELINE
# ==============================================================================

def _validate_function_output(fname: str, lines: List[str], bank: int) -> List[str]:
    """Inspect generated C for red flags that indicate garbled decoding.

    Returns a list of human-readable reasons if the output looks wrong.
    Empty list = output looks plausible.
    """
    reasons = []
    body_lines = [l.strip() for l in lines if l.strip() and not l.strip().startswith(('void ', 'uint8 ', 'uint16 ', 'uint32 ', '//', '{', '}', 'return', 'label_'))]

    if not body_lines:
        return reasons

    # 1. Calls to sub-$8000 addresses (invalid ROM in LoROM)
    sub_calls = [l for l in body_lines if re.search(r'sub_\d{2}_[0-7][0-9a-fA-F]{3}\(', l)]
    if sub_calls:
        reasons.append(f'calls to sub-$8000 ROM addresses ({len(sub_calls)}x) --likely data decoded as code')

    # 2. Calls to func_XXXXXX (unresolved cross-bank --garbled JSL operands)
    # Exclude SCRUBBED comments — those are already neutralized.
    garbled_calls = [l for l in body_lines if re.search(r'func_[0-9a-fA-F]{6}\(', l)
                     and 'SCRUBBED' not in l]
    if garbled_calls:
        reasons.append(f'calls to unresolved func_ addresses ({len(garbled_calls)}x) --garbled JSL operands')

    # 3. High RECOMP_WARN density (>= 4 warnings = suspicious)
    warn_count = sum(1 for l in lines if 'RECOMP_WARN' in l)
    total_stmts = len(body_lines)
    if warn_count >= 4 and total_stmts > 0 and warn_count / max(total_stmts, 1) > 0.15:
        reasons.append(f'high warning density ({warn_count} warnings in {total_stmts} statements)')

    # 4. RAM accesses at nonsensical addresses (>= $3000 indexed by non-sprite offsets)
    nonsense_ram = re.findall(r'g_ram\[0x([0-9a-fA-F]+)\]', '\n'.join(body_lines))
    high_addrs = [int(a, 16) for a in nonsense_ram if int(a, 16) >= 0x3000 and int(a, 16) < 0x7000]
    if len(high_addrs) >= 3:
        # Check if they follow a suspiciously regular pattern (data table artifact)
        high_addrs_sorted = sorted(set(high_addrs))
        if len(high_addrs_sorted) >= 3:
            diffs = [high_addrs_sorted[i+1] - high_addrs_sorted[i] for i in range(len(high_addrs_sorted)-1)]
            if len(set(diffs)) <= 2:  # very regular spacing = data table
                reasons.append(f'regular-spaced high RAM accesses ({len(high_addrs)}x in $3000-$7000) --data table decoded as code')

    # 5. Shift operations on fixed addresses (ASL $XXXX pattern from data bytes)
    # Only flag when multiple DIFFERENT addresses are shifted — repeated
    # shifts to the SAME address are a legit extended-precision idiom
    # (e.g. signed-divide routines: ASL _E; ROL A; ASL _E; ROL A; ...).
    shift_addrs = re.findall(r'g_ram\[0x([0-9a-f]+)\] <<=', '\n'.join(body_lines))
    distinct_shift_addrs = set(shift_addrs)
    if len(shift_addrs) >= 3 and len(distinct_shift_addrs) >= 3:
        reasons.append(f'excessive fixed-address shifts ({len(shift_addrs)}x across {len(distinct_shift_addrs)} addrs) --likely data decoded as ASL instructions')

    # 6. RomPtr with invalid bank numbers (SMW uses banks $00-$0D, $7E-$7F)
    valid_banks = set(range(0x0E)) | {0x7E, 0x7F}
    garbled_rom = re.findall(r'RomPtr_([0-9A-Fa-f]{2})\(', '\n'.join(body_lines))
    invalid_banks = [b for b in garbled_rom if int(b, 16) not in valid_banks]
    if invalid_banks:
        reasons.append(f'RomPtr with invalid banks ({",".join(set(invalid_banks))}) --data decoded as code')

    return reasons


def _classify_dispatch_helper(rom: bytes, bank: int, addr: int) -> Optional[str]:
    """Identify whether the subroutine at (bank, addr) is a dispatch helper
    and, if so, whether its table entries are 16-bit or 24-bit.

    A dispatch helper is an ExecutePtr-style routine that pulls its return
    address off the stack, uses it to index an inline table, and jumps
    through the fetched pointer. Universal 65816 pattern, not game-
    specific, so it can be auto-detected instead of annotated in cfg.

    Signature:
      - body contains PLA or PLY (pulls stacked return address)
      - body ends with an indirect jump: JMP (abs) / JMP (abs,X) / JML [abs]
    Width classification (between the first `ASL A` and the next `TAY`/`TAX`):
      - plain `ASL A; TAY`            -> 'short' (×2, 16-bit entries)
      - `ASL A; <...> ADC <dp>; TAY`  -> 'long'  (×3, 24-bit entries)

    Returns 'short', 'long', or None.
    """
    try:
        insns = decode_func(rom, bank, addr, end=0, validate_branches=False)
    except Exception:
        return None
    if not insns:
        return None
    # Must pull return address off the stack.
    if not any(i.mnem in ('PLA', 'PLY') for i in insns):
        return None
    # Must end with an indirect jump.
    last = insns[-1]
    if not (last.mnem in ('JMP', 'JML') and
            last.mode in (INDIR, INDIR_X, INDIR_L)):
        return None
    # Width: look for ASL A ... TAY/TAX and whether ADC appears between.
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


def _auto_detect_dispatch_helpers(rom: bytes, cfg: Config) -> None:
    """Scan cfg functions for JSL/JML targets, classify each as a dispatch
    helper, and populate cfg.jsl_dispatch / cfg.jsl_dispatch_long
    accordingly. Removes the need for per-bank `jsl_dispatch*` cfg hints
    and catches cfg typos (short/long mixups) by authoritative ROM analysis.

    Explicit cfg hints are preserved (the auto-set is unioned with them).
    """
    cache: Dict[int, Optional[str]] = {}
    jsl_targets: Set[int] = set()
    for fname, start_addr, _sig, eovr, mo, _h in cfg.funcs:
        if fname in cfg.skip:
            continue
        end = eovr if eovr is not None else 0
        try:
            insns = decode_func(rom, cfg.bank, start_addr, end=end,
                                mode_overrides=mo or None,
                                validate_branches=False)
        except Exception:
            continue
        for insn in insns:
            # JSL: 24-bit target already in insn.operand.
            if insn.mnem == 'JSL':
                jsl_targets.add(insn.operand & 0xFFFFFF)
            # JML abs (opcode $5C) is a long unconditional jump — if the
            # target is a known dispatch pattern, treat it the same.
            elif insn.mnem == 'JML' and insn.mode == LONG:
                jsl_targets.add(insn.operand & 0xFFFFFF)

    for tgt in jsl_targets:
        if tgt in cache:
            kind = cache[tgt]
        else:
            tbank = (tgt >> 16) & 0xFF
            taddr = tgt & 0xFFFF
            kind = _classify_dispatch_helper(rom, tbank, taddr)
            cache[tgt] = kind
        if kind == 'short':
            cfg.jsl_dispatch.add(tgt)
        elif kind == 'long':
            cfg.jsl_dispatch_long.add(tgt)


def run_config(rom: bytes, cfg: Config, out_path: Optional[str],
               funcs_h_sigs: Dict[str, str] = None, trace: bool = False,
               prefix: str = '', func_range: Tuple[int, int] = None,
               cfg_path: Optional[str] = None):
    _cfg_path_for_siblings = cfg_path
    lines = []

    # Reconcile cfg sigs with funcs.h using a single rule (see _reconcile_sig).
    # The same effective sig is used for BOTH the C forward declaration and the
    # function body — except for the pointer-return exception, which keeps cfg
    # void for the body but records the funcs.h pointer return type in
    # decl_ret_overrides so the declared function still matches funcs.h.
    decl_ret_overrides: Dict[int, str] = {}
    if funcs_h_sigs:
        for addr, name in cfg.names.items():
            fh_sig = funcs_h_sigs.get(name)
            cfg_sig = cfg.sigs.get(addr)
            reconciled = _reconcile_sig(cfg_sig, fh_sig)
            if reconciled is not None:
                cfg.sigs[addr] = reconciled
            # Pointer-return exception: cfg void wins for body codegen, but
            # the declared return type keeps funcs.h's pointer so that
            # oracle / hand-written callers still see `uint8* Foo()` in the
            # header. See _reconcile_sig docstring.
            if (fh_sig and cfg_sig
                    and _ret_is_pointer(fh_sig)
                    and parse_sig(cfg_sig)[0] == 'void'):
                fh_ret, _ = parse_sig(fh_sig)
                decl_ret_overrides[addr] = fh_ret

    # Header
    guard = f'RECOMP_BANK{cfg.bank:02X}'
    if cfg.comment:
        lines.append(f'// {cfg.comment}')
    lines.append('// Generated by tools/recomp/recomp.py -- DO NOT EDIT')
    lines.append('// Modify the recompiler, then regenerate.')
    lines.append('')

    # Includes
    for inc in cfg.includes:
        lines.append(f'#include "{inc}"')
    if cfg.includes:
        lines.append('')

    # Guard: only compile when RECOMP_BANKXX is defined (skip in prefix/test and range mode)
    if not prefix and func_range is None:
        lines.append(f'#ifdef {guard}')
        lines.append('')

    # ROM data arrays
    for spec in cfg.data:
        lines.append(extract_data_array(
            rom, cfg.bank, spec['addr'], spec['type'], spec['decl'], spec['count']))
        lines.append('')

    # Verbatim and oracle fallback --deferred to after forward declarations

    # ── Intra-bank auto-promote (discover.py) ────────────────────────────
    # Walk the ROM's call graph from every existing cfg func as a seed, and
    # promote every newly-discovered intra-bank JSR/JSL target into a func
    # entry. Without this, targets like $05:86E3 (a local JSR trampoline
    # inside LoadLevelDataObject) emit as unresolved `func_0586e3(...)`
    # calls and get scrubbed, triggering a REVIEW.
    #
    # Scope (intentionally minimal):
    #   * intra-bank targets only (cross-bank JSLs are left to cfg until a
    #     follow-up pass wires them through the global scan)
    #   * skip addresses already in cfg.funcs (explicit)
    #   * skip addresses in cfg.names (will be handled by sub-entry
    #     promotion if they carry a sig; otherwise they're cross-bank alias
    #     declarations that we must not duplicate)
    #   * skip addresses that fall inside any exclude_range (data bytes)
    #   * opt-out: `no_autodiscover ADDR` cfg directive blocks a specific
    #     discovered address from being promoted
    _existing_addrs_pre = {a for _, a, *_ in cfg.funcs}
    _existing_local_names = {a & 0xFFFF for a in cfg.names if (a >> 16) == cfg.bank}
    # Collect incoming cross-bank JSL targets by running discover_bank on
    # each sibling bank (seeded from its own cfg funcs) and taking the
    # cross-bank JSL targets it reports pointing back into this bank.
    # Every target in that output came from a JSL that discover_bank
    # reached through validated code paths — no false positives from
    # random 0x22 bytes in data regions. Cost: O(N) extra discover_bank
    # runs per bank regen, but results are worklist-bounded.
    _incoming_from_siblings: Set[int] = set()
    if _cfg_path_for_siblings:
        _cfg_dir = os.path.dirname(os.path.abspath(_cfg_path_for_siblings))
        _own_base = os.path.basename(_cfg_path_for_siblings)
        import glob as _glob
        for _sib_path in sorted(_glob.glob(os.path.join(_cfg_dir, 'bank*.cfg'))):
            if os.path.basename(_sib_path) == _own_base:
                continue
            try:
                _sib_cfg = parse_config(_sib_path)
            except Exception:
                continue
            _sib_seeds = {a for _, a, *_ in _sib_cfg.funcs}
            try:
                _sib_local, _sib_cross = discover_bank(
                    rom, _sib_cfg.bank,
                    external_seeds=_sib_seeds,
                    jsl_dispatch=set(_sib_cfg.jsl_dispatch or []),
                    jsl_dispatch_long=set(_sib_cfg.jsl_dispatch_long or []),
                )
            except Exception:
                continue
            for _ta in _sib_cross.get(cfg.bank, set()):
                if 0x8000 <= _ta <= 0xFFFF:
                    _incoming_from_siblings.add(_ta)
    # NOTE on incoming cross-bank JSL seeding (historical):
    # We deliberately do NOT seed discover_bank with `scan_for_jsl_targets`
    # output. That scan is a brute-force byte-pattern search for `22 LO HI
    # BANK` across the ROM — it matches real JSLs *and* any data byte
    # sequence that happens to look like one. Feeding those addresses as
    # seeds causes over-promotion of data regions (observed: bank 00 went
    # from 7 to 37 promotions with many RomPtr-invalid-bank REVIEWs).
    # Cross-bank incoming discovery requires a code-path-validated JSL
    # target set — which is what each bank's own outgoing discovery
    # produces (see _discovered_cross below). Proper global cross-bank
    # seeding is a future step: run discover_bank across all banks once,
    # collect code-path-validated jsl_targets, then re-seed each bank with
    # its incoming subset.
    _discovered_local: Set[int] = set()
    _discovered_cross: Dict[int, Set[int]] = {}
    _seed_set = set(_existing_addrs_pre) | _incoming_from_siblings
    # Iterate discovery to fixpoint: each round feeds the newly-found set
    # back in as seeds, so transitively-reachable JSR targets inside
    # discovered functions get traced. discover_bank itself is worklist-
    # based but its linear walker exits at the first RTS/JMP per path and
    # may not follow every branch, so iteration catches the tail.
    for _round in range(8):
        try:
            _round_local, _round_cross = discover_bank(
                rom, cfg.bank,
                external_seeds=_seed_set,
                jsl_dispatch=set(cfg.jsl_dispatch or []),
                jsl_dispatch_long=set(cfg.jsl_dispatch_long or []),
            )
        except Exception as _disc_err:
            print(f'  [auto-promote] discover_bank failed: {_disc_err}',
                  file=sys.stderr)
            break
        _prev_size = len(_discovered_local)
        _discovered_local |= _round_local
        for _tb, _tas in _round_cross.items():
            _discovered_cross.setdefault(_tb, set()).update(_tas)
        if len(_discovered_local) == _prev_size:
            break  # fixpoint
        _seed_set |= _discovered_local
    _auto_promoted = []
    for _local_addr in sorted(_discovered_local):
        if _local_addr < 0x8000 or _local_addr > 0xFFFF:
            continue
        if _local_addr in _existing_addrs_pre:
            continue
        if _local_addr in _existing_local_names:
            continue  # will be handled by sub-entry promotion or is a cross-bank alias
        if _local_addr in cfg.no_autodiscover:
            continue
        in_exclude = False
        for _er_start, _er_end in cfg.exclude_ranges:
            if _er_start <= _local_addr <= _er_end:
                in_exclude = True
                break
        if in_exclude:
            continue
        _auto_name = f'auto_{cfg.bank:02X}_{_local_addr:04X}'
        cfg.funcs.append((_auto_name, _local_addr, 'void()', None, {}, {}))
        _full = (cfg.bank << 16) | _local_addr
        cfg.names[_full] = _auto_name
        cfg.sigs[_full] = 'void()'
        _existing_addrs_pre.add(_local_addr)
        _auto_promoted.append(_local_addr)
    if _auto_promoted:
        cfg.funcs.sort(key=lambda t: t[1])
        print(f'  Auto-promote (intra-bank): {len(_auto_promoted)} JSR/JSL targets promoted',
              file=sys.stderr)
        for _ap in _auto_promoted:
            print(f'    auto_{cfg.bank:02X}_{_ap:04X} @ ${cfg.bank:02X}:{_ap:04X}',
                  file=sys.stderr)

    # ── Cross-bank auto-name (outgoing JSL targets) ──────────────────────
    # When this bank's code does JSL $XX:YYYY to another bank, the emitter
    # needs `cfg.names[(XX<<16)|YYYY]` to resolve the callee to a real C
    # symbol. Sibling-cfg name import (farther down in main) only covers
    # targets that the *other* bank has declared in its own cfg. If the
    # target bank's cfg doesn't declare it (e.g. because it was itself
    # only ever reached via a cross-bank JSL that nobody's cfg names), we
    # get `func_XXXXXX(...)` in the caller and a REVIEW scrub.
    #
    # Register an `auto_<tgt_bank>_<tgt_addr>` name for every discovered
    # outgoing cross-bank JSL target that isn't already named. The target
    # bank's own auto-promote pass (driven by incoming JSL seed) will then
    # emit a matching C definition, so the link resolves.
    _cross_registered = 0
    for _tgt_bank, _tgt_addrs in (_discovered_cross or {}).items():
        if _tgt_bank == cfg.bank:
            continue
        for _tgt_addr in _tgt_addrs:
            if _tgt_addr < 0x8000 or _tgt_addr > 0xFFFF:
                continue
            _tgt_full = (_tgt_bank << 16) | _tgt_addr
            if _tgt_full in cfg.names:
                continue
            _auto_name = f'auto_{_tgt_bank:02X}_{_tgt_addr:04X}'
            cfg.names[_tgt_full] = _auto_name
            if _tgt_full not in cfg.sigs:
                cfg.sigs[_tgt_full] = 'void()'
            _cross_registered += 1
    if _cross_registered:
        print(f'  Auto-promote (cross-bank names): {_cross_registered} outgoing JSL targets named',
              file=sys.stderr)

    # ── Sub-entry promotion ──────────────────────────────────────────────
    # A `name` directive with a `sig:` that falls strictly inside an existing
    # `func`'s address range is a *sub-entry point*: an address that external
    # code (or intra-bank code) can call into the middle of a parent function.
    # See promote_sub_entries for details.
    # Runs BEFORE auto_promote_branch_targets so branch-target inference sees
    # sub-entries as first-class funcs (their internal BRAs to other labels
    # inside the parent range can then be auto-promoted).
    _promoted = promote_sub_entries(rom, cfg)
    if _promoted:
        print(f'  Sub-entry promotion: {len(_promoted)} entries promoted to func:',
              file=sys.stderr)
        for pname, paddr, parent_name, parent_addr in _promoted:
            skipped = ' (skipped)' if pname in cfg.skip else ''
            print(f'    {pname} @ ${cfg.bank:02X}:{paddr:04X}  (parent: {parent_name} @ {parent_addr:04X}){skipped}',
                  file=sys.stderr)

    # ── Auto-promote intra-bank branch targets ────────────────────────────
    # Any BRA/BRL/BCC/JMP etc. whose target lands inside a different known
    # function's range gets a synthetic `auto_BB_AAAA` name + sig so a
    # follow-up sub-entry promotion pass splits that parent and makes the
    # target a callable tail-call destination. Without this, the emitter
    # silently replaces the branch with `return;`, losing any setup the
    # real target would run.
    #
    # Single pass. A prior version iterated to a fixpoint on the theory
    # that each sub-entry promotion could expose more branch targets,
    # but in practice subsequent rounds break the parent function's
    # structure: once a sub-entry is promoted, the parent's remaining
    # body sometimes contains branches to addresses that are now inside
    # the *promoted* sub-function's range rather than the shrunk
    # parent's range. The emitter then demotes those back to "BEQ $XXXX
    # treated as return", which silently drops setup code that was
    # reachable in the original single-function emit. Real crash: the
    # SprXXX_Eeries_Init ... ProcessNormalSprites_HandleSprite ...
    # SprStatus01_Init path segfaulted because the 2nd-round promotion
    # of $01:A9F2 truncated the enclosing SpriteMain function so the
    # BEQ $AA01 in its body became "treat as return", skipping a
    # subsequent fall-through that the dispatched sprite code relied
    # on. Fixing multi-round promotion without that regression is a
    # separate project: the emitter needs to re-decode the parent's
    # remaining body against the post-promotion boundary and emit
    # missing-label branches as tail calls rather than returns.
    for _branch_iter in range(1):
        _auto_branch_promoted = auto_promote_branch_targets(rom, cfg)
        if _auto_branch_promoted:
            print(f'  Auto-promote (intra-bank branch targets, round {_branch_iter + 1}): '
                  f'{_auto_branch_promoted} new names',
                  file=sys.stderr)
            _promoted_round = promote_sub_entries(rom, cfg)
            if _promoted_round:
                print(f'  Sub-entry promotion (round {_branch_iter + 2}): '
                      f'{len(_promoted_round)} additional',
                      file=sys.stderr)
        if _auto_branch_promoted == 0:
            break

    # --- Entry M/X inference from caller context --------------------------
    # Default decode starts every function at M=1,X=1. That's wrong for
    # functions whose callers always run them in 16-bit mode (e.g. helpers
    # called after a REP #$30). Without this, ADC #$xxxx etc. get decoded
    # as 2-byte when the ROM emits 3-byte, producing garbled code that
    # hangs at runtime.
    #
    # Approach: decode each func once, collect M/X at every intra-bank JSR
    # site. If all callers of F agree on (m, x), seed F's entry with an
    # implicit REP. Iterate to fixpoint — a caller's M/X at JSR depends on
    # its own entry state, which this pass refines.
    def _compute_tentative_ends(funcs):
        srt = sorted(funcs, key=lambda t: t[1])
        ends: Dict[int, int] = {}
        for i, tup in enumerate(srt):
            _, saddr, _, eovr, _, _ = tup
            if eovr is not None:
                ends[saddr] = eovr
            elif i + 1 < len(srt):
                ends[saddr] = srt[i + 1][1] - 1
            else:
                ends[saddr] = 0xFFFF
        return ends

    func_entry_addrs = {a for _, a, *_ in cfg.funcs}
    for _iter in range(5):  # fixpoint bound
        ends = _compute_tentative_ends(cfg.funcs)
        callsite_mx: Dict[int, List[Tuple[int, int]]] = {}
        for fname, saddr, _sig, _eovr, mo, _h in cfg.funcs:
            if fname in cfg.skip:
                continue
            try:
                insns = decode_func(rom, cfg.bank, saddr, end=ends[saddr],
                                    mode_overrides=mo or None,
                                    validate_branches=False)
            except (AssertionError, IndexError, Exception):
                continue
            for insn in insns:
                if insn.mnem == 'JSR' and insn.operand in func_entry_addrs:
                    callsite_mx.setdefault(insn.operand, []).append(
                        (insn.m_flag, insn.x_flag))

        changed = False
        new_funcs = []
        for tup in cfg.funcs:
            fname, saddr, sig, eovr, mo, hints = tup
            callers = callsite_mx.get(saddr)
            if not callers:
                new_funcs.append(tup); continue
            ms = {c[0] for c in callers}
            xs = {c[1] for c in callers}
            if len(ms) != 1 or len(xs) != 1:
                new_funcs.append(tup); continue  # mixed — can't decide
            target_m, target_x = ms.pop(), xs.pop()
            want_bits = 0
            if target_m == 0: want_bits |= 0x20
            if target_x == 0: want_bits |= 0x10
            if want_bits == 0:
                new_funcs.append(tup); continue  # callers agree on M=1,X=1 = default
            new_mo = dict(mo) if mo else {}
            old_entry = new_mo.get(saddr, 0)
            # Only set if not already overridden (preserve explicit cfg / sub-entry state).
            if old_entry == 0:
                new_mo[saddr] = want_bits
                changed = True
                new_funcs.append((fname, saddr, sig, eovr, new_mo, hints))
            else:
                new_funcs.append(tup)
        cfg.funcs = new_funcs
        if not changed:
            break

    # Auto-detect ExecutePtr-style dispatch helpers by ROM pattern. Unions
    # with any cfg-provided hints so existing cfgs keep working.
    _auto_detect_dispatch_helpers(rom, cfg)

    # --- Live-in register inference (Rule 0: recompiler is authoritative) --
    # Derive calling-convention parameters directly from the ROM. For each
    # function, decode its body and walk from entry: any of A/X/Y that gets
    # read before being written is live-in, i.e. a parameter. The inferred
    # params are merged into cfg.sigs so both forward declarations and
    # emitted bodies use the augmented sig, and sync_funcs_h.py sees the
    # same result via augment_cfg_sigs_from_livein.
    augment_cfg_sigs_from_livein(rom, cfg)

    # Build function list (excluding skipped)
    funcs_with_end = []
    non_skip = [(f, a, s, e, mo, h) for f, a, s, e, mo, h in cfg.funcs if f not in cfg.skip]
    for i, (fname, start_addr, sig, end_override, mode_ovr, func_hints) in enumerate(non_skip):
        if end_override is not None:
            end_addr = end_override
        elif i + 1 < len(non_skip):
            # Use the NEXT function's start as our exclusive end. Subtracting 1
            # (old behaviour) made end inclusive, which contradicted the
            # MANUAL `end:X` semantics (X is the first address NOT in the
            # function) and caused emit_function (which uses `pc >= end_addr`)
            # to treat the actual last instruction as past-end and skip it.
            # Effect: LoadSublevel's terminating RTL at $809D was never
            # emitted, leaking a RecompStackPush without a Pop.
            end_addr = non_skip[i + 1][1]
        else:
            # Last function in the bank: end is the bank boundary ($10000).
            # Using 0xFFFF would make the instruction at $FFFF unreachable
            # because decode_func uses pc >= end as its exclusive-end check.
            end_addr = 0x10000
        funcs_with_end.append((fname, start_addr, end_addr, sig, mode_ovr, func_hints))

    # Apply prefix to intra-bank function names (for test harness mode).
    # Cross-bank names are left alone so calls to other banks resolve normally.
    # Must happen BEFORE forward declarations so names are correct.
    if prefix:
        intra_bank_addrs = {(cfg.bank << 16) | addr for _, addr, _, _, _, _ in cfg.funcs}
        prefixed_names = {}
        for addr, name in cfg.names.items():
            if addr in intra_bank_addrs:
                prefixed_names[addr] = prefix + name
            else:
                prefixed_names[addr] = name
        cfg.names = prefixed_names
        funcs_with_end = [(prefix + fn, sa, ea, sig, mo, h)
                          for fn, sa, ea, sig, mo, h in funcs_with_end]

    # Apply --range filter: only emit functions within the specified index range
    total_funcs = len(funcs_with_end)
    if func_range is not None:
        range_start, range_end = func_range
        print(f'Emitting functions {range_start}-{range_end} of {total_funcs} (range mode)',
              file=sys.stderr)
        # Collect in-range function names for the range header
        range_func_names = [fn for i, (fn, *_rest) in enumerate(funcs_with_end)
                            if range_start <= i <= range_end]
        funcs_with_end = [f for i, f in enumerate(funcs_with_end)
                          if range_start <= i <= range_end]
        # Also collect function names from verbatim blocks (always emitted)
        verbatim_func_names = []
        for vline in cfg.verbatim:
            vm = re.match(r'(?:void|uint8|uint16|int)\s+(\w+)\s*\(', vline)
            if vm:
                verbatim_func_names.append(vm.group(1))
        # Skip (non-dispatch) funcs are provided by hand-written src/smw_XX.c;
        # declare them in the range header so banks.h marks them externally provided.
        external_skip_names = [s for s in cfg.skip if s not in cfg.dispatch] if cfg.skip else []
        all_range_names = range_func_names + verbatim_func_names + external_skip_names
        # Generate range header --tells consumers which functions this gen file (or its
        # Write to bank_range.h (included via banks.h which is force-included everywhere)
        bank_range_path = os.path.join(os.path.dirname(os.path.abspath(out_path or 'src/gen/dummy')),
                                       'bank_range.h')
        bank_prefix = f'{cfg.bank:02X}'
        with open(bank_range_path, 'w') as rh:
            rh.write(f'// Auto-generated by recomp.py --range {range_start}-{range_end}. DO NOT EDIT.\n')
            rh.write(f'#ifndef BANK_RANGE_H\n')
            rh.write(f'#define BANK_RANGE_H\n\n')
            for fn in all_range_names:
                rh.write(f'#define RECOMP_{bank_prefix}_{fn}\n')
            rh.write(f'\n#endif\n')
        print(f'Wrote bank_range.h: {bank_range_path} ({len(all_range_names)} functions)',
              file=sys.stderr)

    # Forward declarations — use the reconciled sig (same one the body will use).
    # cfg.sigs was updated above by _reconcile_sig, so decl and body agree —
    # except for the pointer-return exception where decl_ret_overrides kicks
    # in to match funcs.h's declared pointer return.
    fwd_lines = []
    for fname, start_addr, end_addr, sig, _mo, _hints in funcs_with_end:
        full_addr = (cfg.bank << 16) | start_addr
        _sig = cfg.sigs.get(full_addr, sig)
        ret_type, params = parse_sig(_sig)
        if full_addr in decl_ret_overrides:
            ret_type = decl_ret_overrides[full_addr]
        param_str = format_param_str(params)
        fwd_lines.append(f'{ret_type} {fname}({param_str});')
    if fwd_lines:
        lines.append('/* Forward declarations for intra-bank functions */')
        lines.extend(fwd_lines)
        lines.append('')

    # Verbatim block (skip in prefix/test mode)
    if not prefix and cfg.verbatim:
        lines.extend(cfg.verbatim)
        lines.append('')

    # Known addresses for dispatch table boundary detection
    known_addrs: Set[int] = set(cfg.names.keys())
    for fname, addr, sig_unused, end_unused, _mo_unused, _h_unused in cfg.funcs:
        known_addrs.add((cfg.bank << 16) | addr)
    _dispatch_known = known_addrs if cfg.jsl_dispatch else known_addrs

    # Generate each function
    for fi, (fname, start_addr, end_addr, sig, mode_ovr, func_hints) in enumerate(funcs_with_end):
        # cfg.sigs has been reconciled with funcs.h, so it's the single source
        # of truth. Prefer it over the stale tuple sig.
        full_addr = (cfg.bank << 16) | start_addr
        reconciled = cfg.sigs.get(full_addr)
        if reconciled is not None:
            sig = reconciled
        insns = decode_func(rom, cfg.bank, start_addr, end=end_addr,
                            jsl_dispatch=cfg.jsl_dispatch or None,
                            jsl_dispatch_long=cfg.jsl_dispatch_long or None,
                            mode_overrides=mode_ovr or None,
                            dispatch_known_addrs=_dispatch_known,
                            exclude_ranges=cfg.exclude_ranges or None,
                            known_func_starts=known_addrs)
        if not insns:
            print(f'  WARN: no instructions decoded for {fname} @ ${cfg.bank:02X}:{start_addr:04X}',
                  file=sys.stderr)
            continue
        # Determine next function for fall-through detection.
        # Prefer cfg.sigs over the stale tuple sig: live-in inference updates
        # cfg.sigs but not the tuple, so the tuple may be missing parameters
        # the callee actually needs.
        next_func = None
        if fi + 1 < len(funcs_with_end):
            nf_name = funcs_with_end[fi + 1][0]
            nf_full = (cfg.bank << 16) | funcs_with_end[fi + 1][1]
            nf_sig = cfg.sigs.get(nf_full, funcs_with_end[fi + 1][3])
            next_func = (nf_name, nf_sig)
        # Also check cfg.names for oracle-only functions at end_addr or end_addr+1
        # (fall-through to skipped/oracle functions)
        if next_func is None:
            for nf_off in [end_addr, end_addr + 1]:
                nf_full = (cfg.bank << 16) | nf_off
                if nf_full in cfg.names:
                    nf_name = cfg.names[nf_full]
                    nf_sig = cfg.sigs.get(nf_full)
                    next_func = (nf_name, nf_sig)
                    break
        # Apply default_init_y if function has j/k param and no explicit init_y
        effective_hints = dict(func_hints)
        if cfg.default_init_y and 'init_y' not in effective_hints:
            _ret, _params = parse_sig(sig)
            if any(pn in ('k', 'j') for _pt, pn in _params):
                effective_hints['init_y'] = cfg.default_init_y
        # Apply inferred carry_ret (populated by augment pass for
        # CLC/SEC-only carry-return helpers).
        if (getattr(cfg, 'carry_ret', None)
                and (cfg.bank << 16 | start_addr) in cfg.carry_ret
                and 'carry_ret' not in effective_hints):
            effective_hints['carry_ret'] = '1'
        full_addr_for_override = (cfg.bank << 16) | start_addr
        func_lines = emit_function(fname, insns, cfg.bank, cfg.names,
                                   func_sigs=cfg.sigs, sig=sig, trace=trace,
                                   next_func=next_func, hints=effective_hints,
                                   dp_sync=cfg.dp_sync, rom=rom,
                                   x_restores_map=cfg.x_restores,
                                   y_after_map=cfg.y_after,
                                   x_after_map=cfg.x_after,
                                   callee_clobbers=getattr(cfg, 'clobbers', None),
                                   end_addr=end_addr,
                                   decl_ret_override=decl_ret_overrides.get(full_addr_for_override))
        # Validation pass: detect obviously garbled output
        review_reasons = _validate_function_output(fname, func_lines, cfg.bank)
        if review_reasons:
            has_garbled = any('garbled' in r or 'unresolved func' in r for r in review_reasons)
            func_lines.insert(1, f'  /* RECOMP_NEEDS_REVIEW: {"; ".join(review_reasons)} */')
            print(f'  REVIEW: {fname} --{"; ".join(review_reasons)}', file=sys.stderr)
            # Scrub garbled lines: replace RomPtr with invalid banks with 0
            scrubbed = []
            for fl in func_lines:
                do_scrub = False
                reason = ''
                if re.search(r'RomPtr_[0-9A-Fa-f]{2}\(', fl):
                    bank_match = re.search(r'RomPtr_([0-9A-Fa-f]{2})\(', fl)
                    if bank_match:
                        bk = int(bank_match.group(1), 16)
                        if bk > 0x0D and bk not in (0x7E, 0x7F):
                            do_scrub = True
                            reason = f'garbled RomPtr_{bank_match.group(1)}'
                if not do_scrub and re.search(r'func_[0-9a-f]{6}\(', fl):
                    do_scrub = True
                    reason = 'unresolved func'
                if do_scrub:
                    # Strip nested comments to avoid /* inside /* */
                    clean = fl.strip().replace('/*', '').replace('*/', '')
                    scrubbed.append(f'  /* SCRUBBED ({reason}): {clean} */')
                    continue
                scrubbed.append(fl)
            func_lines = scrubbed
            # Re-check after scrubbing: if all garbled calls were SCRUBBED
            # (commented out), the function is safe to emit un-guarded.
            post_scrub_reasons = _validate_function_output(fname, func_lines, cfg.bank)
            has_garbled = any('garbled' in r or 'unresolved func' in r for r in post_scrub_reasons)
            if has_garbled:
                guard_name = f'RECOMP_{cfg.bank:02X}_{fname}'
                func_lines.insert(0, f'#if defined({guard_name})  /* garbled -- oracle provides if guard is not defined */')
                func_lines.append(f'#endif // {guard_name}')
                print(f'  GUARDED: {fname} (define {guard_name} to use gen version)', file=sys.stderr)
        lines.extend(func_lines)
        lines.append('')

    # Close the #ifdef guard
    if not prefix and func_range is None:
        lines.append(f'#endif // {guard}')
    lines.append('')

    output = '\n'.join(lines)
    if out_path:
        with open(out_path, 'w') as f:
            f.write(output)
        print(f'Wrote {out_path}')
    else:
        print(output)


def main():
    ap = argparse.ArgumentParser(description='65816 -> C static recompiler')
    ap.add_argument('rom', help='SNES ROM file (.sfc/.smc)')
    ap.add_argument('config', nargs='?', help='Config file')
    ap.add_argument('--output', '-o', help='Output .c file (default: stdout)')
    ap.add_argument('--hexdump', action='store_true')
    ap.add_argument('--disasm', action='store_true')
    ap.add_argument('--bank', type=lambda x: int(x, 16), default=0x07)
    ap.add_argument('--addr', type=lambda x: int(x, 16))
    ap.add_argument('--end', type=lambda x: int(x, 16), default=0)
    ap.add_argument('--len', type=lambda x: int(x, 16), default=0x40)
    ap.add_argument('--trace', action='store_true',
                    help='Emit fprintf trace at function entry')
    ap.add_argument('--prefix', default='',
                    help='Prefix for all emitted function names (e.g. recomp_ for test harness)')
    ap.add_argument('--range', default=None,
                    help='Emit only functions at indices START-END (0-based inclusive, e.g. "0-179"). '
                         'Functions outside the range are skipped entirely (oracle provides them).')
    ap.add_argument('--symbols', default=None,
                    help='JSON symbol file for inline RAM/register name comments '
                         '(generated by parse_smwdisx_symbols.py)')
    args = ap.parse_args()

    rom = load_rom(args.rom)

    # Load optional symbol annotations
    if args.symbols:
        load_symbols(args.symbols)

    if args.hexdump:
        if not args.addr:
            ap.error('--hexdump requires --addr')
        hexdump(rom, args.bank, args.addr, args.len)
        return

    if args.disasm:
        if not args.addr:
            ap.error('--disasm requires --addr')
        disasm(rom, args.bank, args.addr, args.end)
        return

    if not args.config:
        ap.error('config file required (or use --hexdump / --disasm)')

    cfg = parse_config(args.config)

    # Auto-discover sibling bank cfgs and import cross-bank names/sigs.
    # This eliminates the need for manual cross-bank `name` entries —
    # any function declared in bankXX.cfg is automatically visible to
    # all other banks.
    cfg_dir_abs = os.path.dirname(os.path.abspath(args.config))
    cfg_basename = os.path.basename(args.config)
    import glob as _glob
    cross_bank_count = 0
    for sibling in sorted(_glob.glob(os.path.join(cfg_dir_abs, 'bank*.cfg'))):
        if os.path.basename(sibling) == cfg_basename:
            continue
        try:
            sib_cfg = parse_config(sibling)
        except Exception:
            continue
        for addr, name in sib_cfg.names.items():
            bank_of_addr = addr >> 16
            if bank_of_addr == cfg.bank:
                continue
            if addr not in cfg.names:
                cfg.names[addr] = name
                cross_bank_count += 1
            if addr in sib_cfg.sigs and addr not in cfg.sigs:
                cfg.sigs[addr] = sib_cfg.sigs[addr]
    if cross_bank_count:
        print(f'  [global-ns] imported {cross_bank_count} cross-bank names from sibling cfgs',
              file=sys.stderr)

    # Cross-bank clobber propagation
    # -------------------------------
    # augment_cfg_sigs_from_livein populates cfg.clobbers only for the
    # current bank's functions, so every cross-bank JSL falls back to
    # the conservative "callee clobbers A/X/Y" default inside
    # infer_live_in_regs. That kills live-in tracking at the first
    # cross-bank call: a caller whose entry X flows through a JSL to
    # a known-void helper is seen as having X re-defined by the call,
    # so X-as-param-at-entry is lost.
    #
    # Decode each sibling's non-skip functions once, compute the same
    # clobber set the augment pass would, and record them under the
    # full 24-bit address the decoder will emit for intra-sibling
    # calls. From the current bank's perspective these are
    # cross-bank JSL targets, and the lookup in infer_live_in_regs
    # now returns the real preserve-set.
    if not hasattr(cfg, 'clobbers'):
        cfg.clobbers = {}
    if not hasattr(cfg, 'x_restores'):
        cfg.x_restores = {}
    # Global funcname -> full_addr map. Used below to alias cfg.clobbers
    # by C-symbol name resolution (cross-bank name entries may point at
    # trampoline/alias addresses whose clobbers are actually those of a
    # differently-addressed func with the same name, e.g. $01:802A's
    # `HandleNormalSpriteGravity` resolves in C to $01:9032's body).
    funcname_to_addr: Dict[str, int] = {}
    for fname, addr, *_ in cfg.funcs:
        funcname_to_addr[fname] = (cfg.bank << 16) | addr
    xbank_clobber_count = 0
    for sibling in sorted(_glob.glob(os.path.join(cfg_dir_abs, 'bank*.cfg'))):
        sib_base = os.path.basename(sibling)
        if sib_base == cfg_basename:
            continue
        if 'bisect' in sib_base.lower():
            continue
        try:
            sib_cfg = parse_config(sibling)
        except Exception:
            continue
        if sib_cfg.bank is None:
            continue
        for fname, addr, *_ in sib_cfg.funcs:
            funcname_to_addr.setdefault(fname, (sib_cfg.bank << 16) | addr)
        sib_funcs = sorted(sib_cfg.funcs, key=lambda t: t[1])
        for i, (sfname, start_addr, _ssig, eovr, mo, _sh) in enumerate(sib_funcs):
            if sfname in sib_cfg.skip:
                continue
            if eovr is not None:
                end_addr = eovr
            elif i + 1 < len(sib_funcs):
                end_addr = sib_funcs[i + 1][1]
            else:
                end_addr = 0x10000
            full_addr = (sib_cfg.bank << 16) | start_addr
            if full_addr in cfg.clobbers:
                continue  # already recorded (current bank owns this)
            try:
                insns = decode_func(rom, sib_cfg.bank, start_addr, end=end_addr,
                                    jsl_dispatch=sib_cfg.jsl_dispatch or None,
                                    jsl_dispatch_long=sib_cfg.jsl_dispatch_long or None,
                                    mode_overrides=mo or None,
                                    exclude_ranges=sib_cfg.exclude_ranges or None,
                                    validate_branches=False)
            except Exception:
                continue
            if not insns:
                continue
            clob = {reg for reg in ('A', 'X', 'Y')
                    if _writes_register_without_save_restore(insns, reg)}
            x_restore = _detect_x_restore_expr(insns)
            if x_restore:
                cfg.x_restores[full_addr] = x_restore
                clob.discard('X')
            cfg.clobbers[full_addr] = clob
            xbank_clobber_count += 1
        # Import sibling's `preserves` hints — these describe HLE/WRAM
        # callees the sibling bank declared. The current bank may JSL to
        # those addresses too, so we need the preserves data here.
        for p_addr, p_regs in sib_cfg.preserves.items():
            cfg.clobbers[p_addr] = {'A', 'X', 'Y'} - p_regs
    # Apply current cfg's own preserves hints (override any sibling-derived
    # entry, since the owner cfg is authoritative).
    for p_addr, p_regs in cfg.preserves.items():
        cfg.clobbers[p_addr] = {'A', 'X', 'Y'} - p_regs

    # Name-resolution alias pass
    # --------------------------
    # ROM JSL targets are addresses, and cfg.clobbers is address-keyed.
    # But cross-bank `name <addr> <N>` entries sometimes point at an
    # address that has no owning `func` entry (e.g., a JSL trampoline
    # in the owner bank). The C linker resolves those calls by symbol
    # name to the canonical `func N` body at a different address, so
    # the RUNTIME clobbers at the call site are those of the canonical
    # function. Mirror that in cfg.clobbers: for every name entry whose
    # address lacks clobber data, resolve the name to its canonical
    # address and copy the clobber set across.
    alias_count = 0
    for full_addr, name in list(cfg.names.items()):
        if full_addr in cfg.clobbers:
            continue
        canonical = funcname_to_addr.get(name)
        if canonical is None or canonical not in cfg.clobbers:
            continue
        cfg.clobbers[full_addr] = cfg.clobbers[canonical]
        if canonical in cfg.x_restores:
            cfg.x_restores.setdefault(full_addr, cfg.x_restores[canonical])
        alias_count += 1
    if xbank_clobber_count or alias_count:
        print(f'  [xbank-clobbers] imported {xbank_clobber_count} cross-bank clobber sets, '
              f'aliased {alias_count} name-resolved entries',
              file=sys.stderr)

    # Locate funcs.h — prefer output-relative (game project's copy) over
    # cfg-relative (recompiler's potentially stale copy).
    cfg_dir = os.path.dirname(os.path.abspath(args.config))
    funcs_h_sigs = None
    if args.output:
        out_dir = os.path.dirname(os.path.abspath(args.output))
        for inc in cfg.includes:
            if 'funcs.h' in inc:
                candidate = os.path.normpath(os.path.join(out_dir, inc))
                if os.path.exists(candidate):
                    funcs_h_sigs = parse_funcs_h(candidate)
                    break
    if funcs_h_sigs is None:
        for rel in ['../../src/funcs.h', '../src/funcs.h', 'src/funcs.h']:
            candidate = os.path.normpath(os.path.join(cfg_dir, rel))
            if os.path.exists(candidate):
                funcs_h_sigs = parse_funcs_h(candidate)
                break
    if funcs_h_sigs is None:
        for inc in cfg.includes:
            candidate = os.path.normpath(os.path.join(cfg_dir, inc.replace('../', ''), '..', 'funcs.h'))
            if os.path.exists(candidate):
                funcs_h_sigs = parse_funcs_h(candidate)
                break
    if funcs_h_sigs is None and hasattr(cfg, 'extern_decl_paths'):
        for edp in cfg.extern_decl_paths:
            candidate = os.path.normpath(os.path.join(cfg_dir, edp))
            if os.path.exists(candidate) and os.path.basename(candidate) == 'funcs.h':
                funcs_h_sigs = parse_funcs_h(candidate)
                break
    # (output-relative search already tried above as first priority)

    # Parse --range if provided
    func_range = None
    if args.range:
        m = re.match(r'^(\d+)-(\d+)$', args.range)
        if not m:
            ap.error('--range must be in format START-END (e.g. "0-179")')
        func_range = (int(m.group(1)), int(m.group(2)))

    run_config(rom, cfg, args.output, funcs_h_sigs=funcs_h_sigs, trace=args.trace,
               prefix=args.prefix, func_range=func_range, cfg_path=args.config)


if __name__ == '__main__':
    main()
