"""
Scan SMWDisX bank_XX.asm for DATA_XXXXXX regions, compute byte ranges,
and emit cfg-ready `exclude_range` proposals for ranges not already
covered by the cfg.

Why: recompiler cannot statically distinguish inline data from code
within a function's auto end_addr. DATA_ labels in SMWDisX mark those
regions authoritatively. Pre-seeding exclude_range from SMWDisX clears
the class of REVIEWs like "excessive fixed-address shifts" and
"RomPtr with invalid banks" caused by data decoded as code.

Usage:
  python tools/smwdisx_exclude_pass.py <bank_hex>  # e.g. 02
  python tools/smwdisx_exclude_pass.py all

Output is cfg-ready; inspect and paste into bankXX.cfg near existing
`exclude_range` lines. Do not auto-apply.
"""

import os
import re
import sys

SMWDISX = os.path.join(os.path.dirname(__file__), '..', 'SMWDisX')
CFG_DIR = os.path.normpath(os.path.join(
    os.path.dirname(__file__), '..', '..', 'snesrecomp-v2', 'tools', 'recomp'))

LABEL_RE = re.compile(r'^([A-Za-z_][A-Za-z0-9_]*):')
HEX_LABEL_RE = re.compile(r'^(?:CODE|DATA|ADDR|Empty|Return|Fn)_?([0-9A-F]{6}):')

DB_RE = re.compile(r'^\s*(db|dw|dl)\s+(.+?)(?:;.*)?$', re.IGNORECASE)
SKIP_RE = re.compile(r'^\s*skip\s+(\S+)(?:\s|;|$)', re.IGNORECASE)


def _count_args(arg_str: str) -> int:
    # Strip trailing comment and split on commas at depth 0.
    depth = 0
    count = 1
    for ch in arg_str:
        if ch == '(' or ch == '[':
            depth += 1
        elif ch == ')' or ch == ']':
            depth -= 1
        elif ch == ',' and depth == 0:
            count += 1
    return count


def scan_bank(bank_hex: str):
    """Return list of (start_addr, end_addr_inclusive) for DATA_ regions
    in the bank's SMWDisX file, addresses as 16-bit values within the bank.
    """
    path = os.path.join(SMWDISX, f'bank_{bank_hex.upper()}.asm')
    if not os.path.isfile(path):
        # SMWDisX merges some banks (e.g. bank_08-0B.asm)
        for alt in sorted(os.listdir(SMWDISX)):
            if alt.startswith(f'bank_') and alt.endswith('.asm'):
                name = alt[5:-4]
                if '-' in name:
                    lo, hi = name.split('-')
                    if int(lo, 16) <= int(bank_hex, 16) <= int(hi, 16):
                        path = os.path.join(SMWDISX, alt)
                        break
        else:
            return []
    if not os.path.isfile(path):
        return []

    data_regions = []
    cur_data_start = None
    cur_data_name = None
    # Running PC for the current data region; None if we don't have a
    # reliable anchor.
    cur_pc = None

    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        lines = f.readlines()

    def flush(end_addr_exclusive: int):
        nonlocal cur_data_start, cur_data_name, cur_pc
        if cur_data_start is not None and end_addr_exclusive > cur_data_start:
            data_regions.append((cur_data_start, end_addr_exclusive - 1,
                                 cur_data_name))
        cur_data_start = None
        cur_data_name = None
        cur_pc = None

    for line in lines:
        stripped = line.rstrip()
        if not stripped or stripped.lstrip().startswith(';'):
            continue

        m_hex = HEX_LABEL_RE.match(stripped)
        m_any = LABEL_RE.match(stripped)

        if m_hex:
            addr6 = int(m_hex.group(1), 16)
            addr16 = addr6 & 0xFFFF
            # End of any prior DATA region.
            if cur_data_start is not None:
                flush(addr16)
            # Start a new region if this is a DATA_ label.
            if stripped.startswith('DATA_'):
                cur_data_start = addr16
                cur_data_name = m_hex.group(0)[:-1]  # label without colon
                cur_pc = addr16
            continue

        if m_any:
            # Non-hex-encoded label (e.g. PointTile1:). Close any pending
            # region, leave the new label as the boundary.
            if cur_data_start is not None and cur_pc is not None:
                flush(cur_pc)
            continue

        # Inside a region? Track PC by parsing directives.
        if cur_data_start is not None and cur_pc is not None:
            body = line.strip()
            m_db = DB_RE.match(body)
            if m_db:
                kind = m_db.group(1).lower()
                n = _count_args(m_db.group(2))
                size = {'db': 1, 'dw': 2, 'dl': 3}[kind] * n
                cur_pc += size
                continue
            m_skip = SKIP_RE.match(body)
            if m_skip:
                try:
                    n = int(m_skip.group(1), 0)
                except ValueError:
                    # Skip-expr we can't parse: close region conservatively.
                    flush(cur_pc)
                    continue
                cur_pc += n
                continue
            # Anything else (instruction mnemonic, directive) -> close.
            # Exception: leading `if` / `else` / `endif` preprocessor lines
            # that don't emit bytes.
            low = body.lower()
            if low.startswith(('if ', 'else', 'endif', 'elseif')):
                continue
            # Unknown content -> end the region at current pc.
            flush(cur_pc)

    # End of file -> close any open region (unknown end).
    if cur_data_start is not None:
        flush(cur_pc if cur_pc is not None else cur_data_start + 1)

    return data_regions


def load_existing_excludes(bank_hex: str):
    cfg_path = os.path.join(CFG_DIR, f'bank{bank_hex.lower()}.cfg')
    excludes = []
    if not os.path.isfile(cfg_path):
        return excludes
    with open(cfg_path, 'r', encoding='utf-8') as f:
        for line in f:
            line = line.split('#', 1)[0].strip()
            if line.startswith('exclude_range'):
                parts = line.split()
                if len(parts) == 3:
                    try:
                        s = int(parts[1], 16)
                        e = int(parts[2], 16)
                        excludes.append((s, e))
                    except ValueError:
                        pass
    return excludes


def load_func_ranges(bank_hex: str):
    """Return list of (start_addr, end_addr_exclusive) for cfg `func` entries
    in the bank. For AUTO-end funcs, end is inferred as the next func's start.
    """
    cfg_path = os.path.join(CFG_DIR, f'bank{bank_hex.lower()}.cfg')
    entries = []  # (start, explicit_end_or_None, name)
    if not os.path.isfile(cfg_path):
        return []
    with open(cfg_path, 'r', encoding='utf-8') as f:
        for line in f:
            raw = line
            line = line.split('#', 1)[0].strip()
            if not line.startswith('func '):
                continue
            parts = line.split()
            if len(parts) < 3:
                continue
            name = parts[1]
            try:
                start = int(parts[2], 16)
            except ValueError:
                continue
            explicit_end = None
            for tok in parts[3:]:
                if tok.startswith('end:'):
                    try:
                        explicit_end = int(tok[4:], 16)
                    except ValueError:
                        pass
                    break
            entries.append((start, explicit_end, name))
    entries.sort()
    result = []
    for i, (start, expl_end, name) in enumerate(entries):
        if expl_end is not None:
            end = expl_end
        elif i + 1 < len(entries):
            end = entries[i + 1][0]
        else:
            end = 0x10000
        result.append((start, end, name))
    return result


def overlaps_func(s: int, e: int, func_ranges) -> bool:
    for fs, fe, _ in func_ranges:
        if s < fe and e >= fs:
            return True
    return False


def covered(s: int, e: int, excludes) -> bool:
    for es, ee in excludes:
        if es <= s and e <= ee:
            return True
    return False


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    targets = sys.argv[1:]
    if targets == ['all']:
        targets = ['00', '01', '02', '03', '04', '05', '06', '07',
                   '08', '09', '0A', '0B', '0C', '0D', '0E', '0F']

    for bank in targets:
        regions = scan_bank(bank)
        if not regions:
            continue
        existing = load_existing_excludes(bank)
        func_ranges = load_func_ranges(bank)
        new_regions = [(s, e, n) for (s, e, n) in regions
                       if not covered(s, e, existing)
                       and overlaps_func(s, e, func_ranges)]
        orphan_count = sum(1 for (s, e, n) in regions
                           if not covered(s, e, existing)
                           and not overlaps_func(s, e, func_ranges))
        if not new_regions:
            print(f'# bank {bank}: all DATA regions either covered or orphan '
                  f'(covered={len(regions)-orphan_count}, orphan={orphan_count})')
            continue
        print(f'# bank {bank}: {len(new_regions)} new exclude_range proposals '
              f'within cfg func ranges (of {len(regions)} total, {orphan_count} orphan)')
        for s, e, n in new_regions:
            size = e - s + 1
            # Which func each DATA falls in.
            owner = next((fn for (fs, fe, fn) in func_ranges if fs <= s < fe), '?')
            print(f'exclude_range {s:04X} {e:04X}  # {n} ({size}B) in {owner}')
        print()


if __name__ == '__main__':
    main()
