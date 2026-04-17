#!/usr/bin/env python3
"""
tools/recomp/discover.py -- Static function discovery for SNES ROMs

Discovers function boundaries from ROM bytes using call graph traversal.
No oracle required — works from JSR/JSL targets, SNES vectors, and jump tables.

Usage:
    python discover.py ROM.sfc --bank 07
    python discover.py ROM.sfc --all
    python discover.py ROM.sfc --all --compare bank07.cfg
"""

import argparse
import os
import re
import sys
from typing import Dict, Set, Tuple, Optional, List

from snes65816 import (
    load_rom, lorom_offset, rom_slice, decode_insn, validate_decoded_insns,
    Insn, IMP, ACC, IMM, DP, ABS, ABS_X, LONG, LONG_X, REL, REL16,
    INDIR, INDIR_X, INDIR_Y, INDIR_LY, INDIR_L,
)

# ==============================================================================
# SNES VECTOR TABLE
# ==============================================================================

# Native mode vectors (bank $00)
SNES_VECTORS = {
    0xFFE4: 'COP',
    0xFFE6: 'BRK',
    0xFFEA: 'NMI',
    0xFFFC: 'RESET',
    0xFFFE: 'IRQ',
}


def read_vectors(rom: bytes) -> Dict[str, int]:
    """Read SNES interrupt vectors from bank $00."""
    vectors = {}
    for vec_addr, name in SNES_VECTORS.items():
        off = lorom_offset(0, vec_addr)
        target = rom[off] | (rom[off + 1] << 8)
        if 0x8000 <= target <= 0xFFFF:
            vectors[name] = target
    return vectors


# ==============================================================================
# FUNCTION DISCOVERY
# ==============================================================================

def discover_bank(rom: bytes, bank: int, external_seeds: Set[int] = None,
                  max_banks: int = 0x0E,
                  jsl_dispatch: Set[int] = None,
                  jsl_dispatch_long: Set[int] = None) -> Set[int]:
    """Discover function start addresses in a single bank.

    Args:
        rom: Full ROM data
        bank: Bank number to analyze (0x00-0x0D)
        external_seeds: Additional seed addresses from cross-bank JSL scan
        max_banks: Maximum valid bank number for JSL validation
        jsl_dispatch: Set of 24-bit addresses for inline 2-byte dispatch tables
        jsl_dispatch_long: Set of 24-bit addresses for inline 3-byte dispatch tables

    Returns:
        Set of discovered function start addresses (bank-local, $8000-$FFFF)
    """
    if jsl_dispatch is None: jsl_dispatch = set()
    if jsl_dispatch_long is None: jsl_dispatch_long = set()
    bank_size = 0x8000
    bank_off = lorom_offset(bank, 0x8000)
    bank_data = rom[bank_off:bank_off + bank_size]

    discovered: Set[int] = set()       # confirmed function starts
    worklist: List[int] = []           # addresses to decode
    decoded_ranges: List[Tuple[int, int]] = []  # (start, end) of decoded regions
    jsr_targets: Set[int] = set()      # JSR targets found during traversal
    jsl_targets: Dict[int, Set[int]] = {}  # bank -> set of JSL targets

    def add_seed(addr: int):
        if 0x8000 <= addr <= 0xFFFF and addr not in discovered:
            discovered.add(addr)
            worklist.append(addr)

    # Phase 1: Seed entry points
    if bank == 0:
        vectors = read_vectors(rom)
        for name, addr in vectors.items():
            add_seed(addr)

    if external_seeds:
        for addr in external_seeds:
            add_seed(addr)

    # Phase 2: If no seeds yet, do a brute-force JSL scan across all banks
    # to find cross-bank calls INTO this bank
    if not worklist:
        cross_bank_targets = scan_for_jsl_targets(rom, bank, max_banks)
        for addr in cross_bank_targets:
            add_seed(addr)

    # If still no seeds, try starting at $8000 (many banks start there)
    if not worklist:
        add_seed(0x8000)

    # Phase 3: Worklist-based traversal
    decoded_pcs: Set[int] = set()  # PCs we've already decoded (avoid re-decode)

    while worklist:
        func_start = worklist.pop()
        if func_start in decoded_pcs:
            continue

        # Path-based BFS: seed with (func_start, m=1, x=1); each popped path
        # walks linearly until a terminator, pushing conditional-branch targets
        # back onto the queue. Ensures every reachable byte in the function is
        # decoded, not just the first linear path plus one level of fanout.
        func_max_pc = func_start
        pending_paths: List[Tuple[int, int, int]] = [(func_start, 1, 1)]
        path_safety = 0

        while pending_paths and path_safety < 10000:
            path_safety += 1
            pc, m, x = pending_paths.pop()
            if pc in decoded_pcs and pc != func_start:
                continue

            linear_safety = 0
            while pc < 0x10000 and linear_safety < 5000:
                linear_safety += 1
                if pc in decoded_pcs and pc != func_start:
                    break

                off = pc - 0x8000
                if off < 0 or off + 4 >= len(bank_data):
                    break

                insn = decode_insn(bank_data, off, pc, bank, m, x)
                if insn is None:
                    break

                decoded_pcs.add(pc)
                func_max_pc = max(func_max_pc, pc + insn.length - 1)
                pc += insn.length

                if insn.mnem == 'REP':
                    if insn.operand & 0x20: m = 0
                    if insn.operand & 0x10: x = 0
                elif insn.mnem == 'SEP':
                    if insn.operand & 0x20: m = 1
                    if insn.operand & 0x10: x = 1

                if insn.mnem == 'JSR' and insn.mode == ABS:
                    target = insn.operand
                    if 0x8000 <= target <= 0xFFFF:
                        jsr_targets.add(target)
                        add_seed(target)

                elif insn.mnem == 'JSL' and insn.mode == LONG:
                    tgt_bank = (insn.operand >> 16) & 0xFF
                    tgt_addr = insn.operand & 0xFFFF
                    if tgt_bank <= max_banks and 0x8000 <= tgt_addr <= 0xFFFF:
                        if tgt_bank not in jsl_targets:
                            jsl_targets[tgt_bank] = set()
                        jsl_targets[tgt_bank].add(tgt_addr)
                        if tgt_bank == bank:
                            add_seed(tgt_addr)

                    _is_short = insn.operand in jsl_dispatch
                    _is_long = insn.operand in jsl_dispatch_long
                    if _is_short or _is_long:
                        entry_size = 3 if _is_long else 2
                        tbl_pc = pc
                        for _ in range(256):
                            tbl_off = tbl_pc - 0x8000
                            if tbl_off + entry_size > len(bank_data):
                                break
                            lo = bank_data[tbl_off]
                            hi = bank_data[tbl_off + 1]
                            entry = lo | (hi << 8)
                            if _is_long:
                                eb = bank_data[tbl_off + 2]
                                if entry < 0x8000 or eb != bank:
                                    break
                            else:
                                if entry < 0x8000:
                                    break
                            add_seed(entry)
                            tbl_pc += entry_size
                        pc = tbl_pc
                        decoded_pcs.update(range(insn.addr & 0xFFFF, tbl_pc))
                        continue

                elif insn.mnem == 'JMP' and insn.mode == ABS:
                    target = insn.operand
                    if 0x8000 <= target <= 0xFFFF:
                        if target in discovered:
                            add_seed(target)
                        elif target not in decoded_pcs:
                            pending_paths.append((target, m, x))
                    break

                elif insn.mnem == 'JMP' and insn.mode == LONG:
                    tgt_bank = (insn.operand >> 16) & 0xFF
                    tgt_addr = insn.operand & 0xFFFF
                    if tgt_bank <= max_banks and 0x8000 <= tgt_addr <= 0xFFFF:
                        if tgt_bank not in jsl_targets:
                            jsl_targets[tgt_bank] = set()
                        jsl_targets[tgt_bank].add(tgt_addr)
                        if tgt_bank == bank:
                            add_seed(tgt_addr)
                    break

                elif insn.mnem == 'JMP' and insn.mode == INDIR_X:
                    table_addr = insn.operand
                    if 0x8000 <= table_addr <= 0xFFFF:
                        entries = read_jump_table(bank_data, table_addr - 0x8000, func_start)
                        for entry in entries:
                            add_seed(entry)
                    break

                elif insn.mnem in ('BPL','BMI','BEQ','BNE','BCC','BCS','BVC','BVS'):
                    target = insn.operand
                    if 0x8000 <= target <= 0xFFFF and target >= func_start:
                        if target not in decoded_pcs:
                            pending_paths.append((target, m, x))

                elif insn.mnem == 'BRA':
                    target = insn.operand
                    if 0x8000 <= target <= 0xFFFF:
                        if target not in decoded_pcs:
                            pending_paths.append((target, m, x))
                    break

                elif insn.mnem == 'BRL':
                    target = insn.operand
                    if 0x8000 <= target <= 0xFFFF:
                        if target not in decoded_pcs:
                            pending_paths.append((target, m, x))
                    break

                elif insn.mnem in ('RTS', 'RTL', 'RTI'):
                    break

        decoded_ranges.append((func_start, func_max_pc))

    return discovered, jsl_targets


def scan_for_jsl_targets(rom: bytes, target_bank: int, max_banks: int = 0x0E) -> Set[int]:
    """Scan all banks for JSL instructions targeting the given bank."""
    targets = set()
    for scan_bank in range(max_banks):
        try:
            bank_off = lorom_offset(scan_bank, 0x8000)
        except (AssertionError, Exception):
            continue
        bank_data = rom[bank_off:bank_off + 0x8000]
        # Look for JSL opcode (0x22) followed by target bank byte
        for i in range(len(bank_data) - 3):
            if bank_data[i] == 0x22:  # JSL
                tgt_lo = bank_data[i + 1]
                tgt_hi = bank_data[i + 2]
                tgt_bk = bank_data[i + 3]
                if tgt_bk == target_bank:
                    addr = tgt_lo | (tgt_hi << 8)
                    if 0x8000 <= addr <= 0xFFFF:
                        targets.add(addr)
    return targets


def read_jump_table(bank_data: bytes, table_off: int, func_start: int,
                    max_entries: int = 64) -> List[int]:
    """Read 16-bit function pointer entries from a jump table.

    Stops when an entry falls outside the valid code range.
    """
    entries = []
    for i in range(max_entries):
        off = table_off + i * 2
        if off + 1 >= len(bank_data):
            break
        addr = bank_data[off] | (bank_data[off + 1] << 8)
        if addr < 0x8000 or addr > 0xFFFF:
            break
        entries.append(addr)
    return entries


# ==============================================================================
# ORACLE COMPARISON
# ==============================================================================

def load_oracle_cfg(path: str) -> Tuple[int, Set[int], Dict[int, str]]:
    """Parse an oracle cfg file to extract function addresses and names.

    Returns (bank, set_of_addresses, {addr: name})
    """
    bank = None
    funcs: Dict[int, str] = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#') or line.startswith('//'):
                continue
            parts = line.split()
            if not parts:
                continue
            key = parts[0]
            if key == 'bank':
                bank = int(parts[2], 16)
            elif key == 'func' and len(parts) >= 3:
                name = parts[1]
                addr = int(parts[2], 16)
                funcs[addr] = name
            elif key == 'name' and len(parts) >= 3:
                full_addr = int(parts[1], 16)
                name = parts[2]
                addr = full_addr & 0xFFFF
                if bank is None:
                    bank = (full_addr >> 16) & 0xFF
                funcs[addr] = name
    return bank, set(funcs.keys()), funcs


def compare_discovery(discovered: Set[int], oracle_addrs: Set[int],
                      oracle_names: Dict[int, str]) -> None:
    """Print coverage comparison between discovered and oracle function sets."""
    found = discovered & oracle_addrs
    missed = oracle_addrs - discovered
    extra = discovered - oracle_addrs

    total = len(oracle_addrs)
    print(f"\n=== Discovery Coverage ===")
    print(f"Oracle functions: {total}")
    print(f"Discovered:       {len(discovered)}")
    print(f"Found (match):    {len(found)} / {total} ({100*len(found)/total:.1f}%)")
    print(f"Missed:           {len(missed)}")
    print(f"Extra (no oracle):{len(extra)}")

    if missed:
        print(f"\n--- Missed functions ({len(missed)}) ---")
        for addr in sorted(missed):
            name = oracle_names.get(addr, f'sub_{addr:04X}')
            print(f"  ${addr:04X}  {name}")

    if extra:
        print(f"\n--- Extra discovered (not in oracle, {len(extra)}) ---")
        for addr in sorted(extra):
            print(f"  ${addr:04X}")


# ==============================================================================
# CFG ANNOTATION
# ==============================================================================

def annotate_cfg(cfg_path: str, discovered: Set[int]) -> Tuple[int, int]:
    """Annotate func lines in a cfg file with # AUTO or # MANUAL.

    Returns (auto_count, manual_count).
    """
    with open(cfg_path) as f:
        lines = f.readlines()

    auto_count = 0
    manual_count = 0
    out = []
    func_re = re.compile(r'^(func\s+\S+\s+)([0-9a-fA-F]+)(.*?)(\s*#\s*(AUTO|MANUAL))?\s*$')

    for line in lines:
        m = func_re.match(line)
        if m:
            prefix = m.group(1)       # "func NAME "
            addr_str = m.group(2)     # hex address
            rest = m.group(3)         # hints after address
            addr = int(addr_str, 16)

            # Strip any existing AUTO/MANUAL annotation
            rest = re.sub(r'\s*#\s*(AUTO|MANUAL)\s*$', '', rest)

            if addr in discovered:
                tag = '# AUTO'
                auto_count += 1
            else:
                tag = '# MANUAL'
                manual_count += 1

            out.append(f'{prefix}{addr_str}{rest} {tag}\n')
        else:
            out.append(line)

    with open(cfg_path, 'w') as f:
        f.writelines(out)

    return auto_count, manual_count


# ==============================================================================
# MAIN
# ==============================================================================

def main():
    parser = argparse.ArgumentParser(description='Discover function boundaries in SNES ROM')
    parser.add_argument('rom', help='Path to SNES ROM file')
    parser.add_argument('--bank', type=lambda x: int(x, 16),
                        help='Bank number to analyze (hex, e.g. 07)')
    parser.add_argument('--all', action='store_true',
                        help='Analyze all banks $00-$0D')
    parser.add_argument('--compare', metavar='CFG',
                        help='Compare against oracle cfg file')
    parser.add_argument('--max-bank', type=lambda x: int(x, 16), default=0x0E,
                        help='Maximum valid bank number (default: 0E)')
    parser.add_argument('--dispatch', type=lambda x: int(x, 16), action='append', default=[],
                        help='JSL dispatch address (24-bit hex, 2-byte inline entries)')
    parser.add_argument('--dispatch-long', type=lambda x: int(x, 16), action='append', default=[],
                        help='JSL dispatch address (24-bit hex, 3-byte inline entries)')
    parser.add_argument('--annotate', metavar='CFG',
                        help='Annotate func lines in cfg with # AUTO / # MANUAL')
    args = parser.parse_args()

    rom = load_rom(args.rom)

    if args.all:
        banks = list(range(args.max_bank))
    elif args.bank is not None:
        banks = [args.bank]
    else:
        parser.error('Specify --bank or --all')
        return

    disp_short = set(args.dispatch)
    disp_long = set(args.dispatch_long)

    # Multi-pass: first discover bank $00 (has vectors), then use JSL targets
    # from bank $00 to seed other banks
    all_discovered: Dict[int, Set[int]] = {}
    all_jsl_targets: Dict[int, Set[int]] = {}

    # Pass 1: discover each bank independently
    for bank in banks:
        seeds = all_jsl_targets.get(bank, set())
        discovered, jsl_targets = discover_bank(
            rom, bank, seeds, args.max_bank, disp_short, disp_long)
        all_discovered[bank] = discovered
        # Merge JSL targets
        for tgt_bank, addrs in jsl_targets.items():
            if tgt_bank not in all_jsl_targets:
                all_jsl_targets[tgt_bank] = set()
            all_jsl_targets[tgt_bank].update(addrs)

    # Pass 2: re-discover banks that gained new seeds from Pass 1
    for bank in banks:
        new_seeds = all_jsl_targets.get(bank, set()) - all_discovered.get(bank, set())
        if new_seeds:
            discovered, jsl_targets = discover_bank(
                rom, bank, all_discovered[bank] | new_seeds, args.max_bank,
                disp_short, disp_long)
            all_discovered[bank] = discovered

    # Output
    for bank in sorted(all_discovered.keys()):
        addrs = sorted(all_discovered[bank])
        print(f"\n# Bank ${bank:02X}: {len(addrs)} functions discovered")
        for addr in addrs:
            print(f"func sub_{bank:02X}{addr:04X} {addr:04X}")

    # Compare against oracle if requested
    if args.compare:
        oracle_bank, oracle_addrs, oracle_names = load_oracle_cfg(args.compare)
        if oracle_bank is not None and oracle_bank in all_discovered:
            compare_discovery(all_discovered[oracle_bank], oracle_addrs, oracle_names)
        else:
            # Try to match by the single bank we analyzed
            if len(banks) == 1:
                compare_discovery(all_discovered[banks[0]], oracle_addrs, oracle_names)
            else:
                print("Could not determine which bank to compare against oracle")

    # Annotate cfg if requested
    if args.annotate:
        if len(banks) == 1:
            disc = all_discovered[banks[0]]
        else:
            # Merge all discovered addresses (annotate expects bank-local)
            disc = set()
            for addrs in all_discovered.values():
                disc.update(addrs)
        auto, manual = annotate_cfg(args.annotate, disc)
        print(f"\nAnnotated {args.annotate}: {auto} AUTO, {manual} MANUAL")


if __name__ == '__main__':
    main()
