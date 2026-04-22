#!/usr/bin/env python3
"""
cfg_apply_audit_fixes — apply every RECOMP_WARN-suggested fix from
cfg_audit's ACTIVE BUGS section directly to the cfg files.

Each RECOMP_WARN of shape:
  /* RECOMP_WARN: BCS $A84C treated as return ... 'end:A84C' ... */

Has a precise fix: add `end:A84C` to the enclosing func line in
recomp/bank<N>.cfg. This script does that for every active warn.

Idempotent: won't double-apply. If end: is already present (with the
same or larger address), skip.

Usage:
    python snesrecomp/tools/cfg_apply_audit_fixes.py [--dry-run]
"""
import argparse
import pathlib
import re
import sys
from typing import Dict, List, Tuple

REPO = pathlib.Path(__file__).resolve().parent.parent
PARENT = pathlib.Path('F:/Projects/SuperMarioWorldRecomp')
CFG_DIR = PARENT / 'recomp'
GEN_DIR = PARENT / 'src' / 'gen'

WARN_RE = re.compile(
    r'/\* RECOMP_WARN:\s+(\S+)\s+\$([0-9a-fA-F]+)\s+treated as return.*?'
    r"Fix:.*?'end:([0-9a-fA-F]+)'",
    re.DOTALL,
)
FUNC_DEF_RE = re.compile(r'^\s*\S+\s+(\w+)\s*\([^)]*\)\s*\{\s*//\s*([0-9a-fA-F]+)')
CFG_ADDR_RE = re.compile(r'^(?:func|name)\s+\S+\s+([0-9a-fA-F]{4,6})', re.MULTILINE)


def _bank_symbols(bank: int) -> List[int]:
    cp = CFG_DIR / f'bank{bank:02x}.cfg'
    if not cp.exists(): return []
    addrs = set()
    for m in CFG_ADDR_RE.finditer(cp.read_text(encoding='utf-8', errors='ignore')):
        addrs.add(int(m.group(1), 16) & 0xFFFF)
    return sorted(addrs)


def _next_symbol_after(symbols: List[int], addr: int) -> int:
    for v in symbols:
        if v > addr: return v
    return 0x10000


def collect_active_fixes() -> List[Tuple[int, int, int]]:
    """Returns list of (bank, func_start_addr, fix_end_addr).

    fix_end is computed as the next defined cfg symbol > branch_target,
    NOT the recompiler's `Fix: end:{branch_target}` suggestion (which is
    off-by-one because end: is exclusive — branch_target itself stays
    outside the decoded range).
    """
    fixes: List[Tuple[int, int, int]] = []
    for bank in (0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x07, 0x0c, 0x0d):
        gp = GEN_DIR / f'smw_{bank:02x}_gen.c'
        if not gp.exists(): continue
        text = gp.read_text(encoding='utf-8', errors='ignore')
        symbols = _bank_symbols(bank)
        defs = []
        for i, line in enumerate(text.splitlines()):
            m = FUNC_DEF_RE.match(line)
            if m:
                defs.append((i, int(m.group(2), 16)))
        for warn in WARN_RE.finditer(text):
            br_target = int(warn.group(2), 16)
            fix_end = _next_symbol_after(symbols, br_target)
            warn_line = text.count('\n', 0, warn.start())
            enclosing_addr = None
            for line_no, addr in defs:
                if line_no <= warn_line:
                    enclosing_addr = addr
                else:
                    break
            if enclosing_addr is not None:
                fixes.append((bank, enclosing_addr, fix_end))
    return fixes


def apply_fixes_to_cfg(cfg_path: pathlib.Path, fixes_for_bank: Dict[int, int],
                      bank: int, dry_run: bool) -> Tuple[int, int, int]:
    """Returns (applied_count, skipped_count, added_count). `added` is
    new cfg entries created when the discoverer found a function but
    no cfg `func` line existed for it (auto_NN_HHHH cases)."""
    applied = 0
    skipped = 0
    added = 0
    seen_addrs: Dict[int, int] = {}  # cfg-addr -> line index for in-place edits

    out_lines: List[str] = []
    # Preserve original line ending. Read in binary to detect CRLF vs LF.
    raw_bytes = cfg_path.read_bytes()
    newline = b'\r\n' if b'\r\n' in raw_bytes else b'\n'
    text = raw_bytes.decode('utf-8', errors='replace')
    raw_lines = text.split(newline.decode('utf-8'))
    # split() leaves a trailing '' if the file ended with newline.
    trailing_newline = raw_lines and raw_lines[-1] == ''
    if trailing_newline:
        raw_lines = raw_lines[:-1]

    for idx, line in enumerate(raw_lines):
        stripped = line.strip()
        # Strip inline comment for tokenization — but preserve the
        # original line text we'll either keep or rewrite.
        comment_idx = stripped.find(' #')
        body = stripped[:comment_idx] if comment_idx >= 0 else stripped
        comment = stripped[comment_idx:] if comment_idx >= 0 else ''
        tokens = body.split()
        if not tokens or tokens[0] != 'func' or len(tokens) < 3:
            out_lines.append(line); continue
        # tokens: ['func', name, addr, hint, hint, ...]
        try:
            addr = int(tokens[2], 16) & 0xFFFF
        except ValueError:
            out_lines.append(line); continue
        seen_addrs[addr] = idx
        if addr not in fixes_for_bank:
            out_lines.append(line); continue
        fix_end = fixes_for_bank[addr]
        # Already has end:? Upgrade if too small, skip if already >= fix_end.
        end_idx = next((i for i, t in enumerate(tokens) if t.startswith('end:')), -1)
        if end_idx >= 0:
            try:
                cur_end = int(tokens[end_idx][len('end:'):], 16)
            except ValueError:
                cur_end = 0
            if cur_end >= fix_end:
                skipped += 1
                out_lines.append(line); continue
            # Replace in place.
            tokens[end_idx] = f'end:{fix_end:x}'
            new_tokens = tokens
        else:
            # Insert end:XXXX right after the address (position 2 in tokens).
            new_tokens = tokens[:3] + [f'end:{fix_end:x}'] + tokens[3:]
        # Preserve leading whitespace (cfg uses column-aligned layout).
        leading = line[:len(line) - len(line.lstrip())]
        new_line = leading + ' '.join(new_tokens)
        if comment:
            new_line += '  ' + comment
        out_lines.append(new_line)
        applied += 1

    # Add cfg entries for fixes that have NO existing cfg line. These
    # are bare auto_NN_HHHH functions the discoverer found but the cfg
    # author never named/bounded.
    for addr, fix_end in fixes_for_bank.items():
        if addr in seen_addrs: continue
        # Synthesize a cfg line. Use the auto_BB_AAAA naming convention
        # so the gen-C symbol stays the same.
        name = f'auto_{bank:02x}_{addr:04X}'
        out_lines.append(f'func {name} {addr:x} end:{fix_end:x} sig:void()  # AUDIT_FIX: created from RECOMP_WARN audit')
        added += 1

    if not dry_run:
        nl = newline.decode('utf-8')
        body = nl.join(out_lines) + (nl if trailing_newline else '')
        cfg_path.write_bytes(body.encode('utf-8'))
    return applied, skipped, added


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument('--dry-run', action='store_true')
    args = p.parse_args()

    fixes = collect_active_fixes()
    if not fixes:
        print('No RECOMP_WARN entries to fix. Nothing to do.')
        return 0

    print(f'Found {len(fixes)} RECOMP_WARN-driven fixes to apply:')
    by_bank: Dict[int, Dict[int, int]] = {}
    for bank, faddr, fend in fixes:
        # Store bank-local addresses to match cfg's hex format.
        by_bank.setdefault(bank, {})[faddr & 0xFFFF] = fend & 0xFFFF
        print(f'  bank {bank:02x}  func ${faddr:06x}  +end:{fend:x}')
    print()

    grand_applied = 0
    grand_skipped = 0
    grand_added = 0
    for bank, bank_fixes in by_bank.items():
        cfg_path = CFG_DIR / f'bank{bank:02x}.cfg'
        if not cfg_path.exists():
            print(f'  bank {bank:02x}: cfg not found, skipping'); continue
        applied, skipped, added = apply_fixes_to_cfg(cfg_path, bank_fixes, bank, args.dry_run)
        verb = 'would apply' if args.dry_run else 'applied'
        print(f'  bank {bank:02x}: {verb} {applied}, added {added}, skipped {skipped} '
              f'(of {len(bank_fixes)} pending)')
        grand_applied += applied
        grand_skipped += skipped
        grand_added += added

    print()
    if args.dry_run:
        print(f'DRY-RUN: would apply {grand_applied}, add {grand_added}, skip {grand_skipped}.')
    else:
        print(f'Applied {grand_applied}, added {grand_added}, skipped {grand_skipped}.')
        print('Now regen all banks and rebuild Oracle to verify the warnings disappear.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
