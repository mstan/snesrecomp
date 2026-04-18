"""
Verify cfg `func` boundaries against SMWDisX labeled structure.

For each cfg func range [start, end), scan SMWDisX for CODE_XXXXXX,
ADDR_XXXXXX, or named labels that fall strictly inside the range.
These represent either:
  - a sub-function entry point missing from cfg (should be promoted), or
  - a legitimate branch target inside the function (ignore).

Heuristic: a label is a likely missing func entry if it's the target
of a JSR/JSL (not just a branch). This script reports candidates; it
does not auto-apply.

Usage:
  python tools/smwdisx_boundary_check.py <bank_hex>
  python tools/smwdisx_boundary_check.py all
"""

import os
import re
import sys

SMWDISX = os.path.join(os.path.dirname(__file__), '..', 'SMWDisX')
CFG_DIR = os.path.normpath(os.path.join(
    os.path.dirname(__file__), '..', '..', 'snesrecomp-v2', 'tools', 'recomp'))

HEX_LABEL_RE = re.compile(r'^(CODE|ADDR|Fn|Return)_([0-9A-F]{6}):')
NAMED_LABEL_RE = re.compile(r'^([A-Za-z_][A-Za-z0-9_]*):')
JSR_JSL_RE = re.compile(r'^\s*(?:JSR|JSL)\s+([A-Za-z_][A-Za-z0-9_]*|\$[0-9A-Fa-f]+|CODE_[0-9A-F]{6})', re.IGNORECASE)


def asm_path(bank_hex: str):
    path = os.path.join(SMWDISX, f'bank_{bank_hex.upper()}.asm')
    if os.path.isfile(path):
        return path
    for alt in sorted(os.listdir(SMWDISX)):
        if alt.startswith('bank_') and alt.endswith('.asm'):
            name = alt[5:-4]
            if '-' in name:
                lo, hi = name.split('-')
                if int(lo, 16) <= int(bank_hex, 16) <= int(hi, 16):
                    return os.path.join(SMWDISX, alt)
    return None


def scan_smwdisx_labels_and_calls(bank_hex: str):
    """Returns:
      addressed_labels: list of (addr16, name) for CODE_/ADDR_/Fn_/Return_.
      all_call_targets: set of names appearing as JSR/JSL targets.
    """
    path = asm_path(bank_hex)
    if not path:
        return [], set()
    bank_int = int(bank_hex, 16)
    addressed = []
    call_targets = set()
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        for line in f:
            m_hex = HEX_LABEL_RE.match(line)
            if m_hex:
                addr6 = int(m_hex.group(2), 16)
                if (addr6 >> 16) == bank_int:
                    addressed.append((addr6 & 0xFFFF, m_hex.group(0)[:-1]))
                continue
            m_call = JSR_JSL_RE.match(line)
            if m_call:
                tgt = m_call.group(1)
                # Strip $ prefix for hex addresses
                if tgt.startswith('$'):
                    continue  # raw hex target, not a symbolic name
                call_targets.add(tgt)
    return addressed, call_targets


def load_cfg_funcs(bank_hex: str):
    """Returns list of (start, end_inclusive, name, source) where source
    is 'explicit' (end: was specified) or 'auto' (next func's start - 1).
    """
    cfg_path = os.path.join(CFG_DIR, f'bank{bank_hex.lower()}.cfg')
    entries = []
    if not os.path.isfile(cfg_path):
        return []
    with open(cfg_path, 'r', encoding='utf-8') as f:
        for line in f:
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
            entries.append([start, explicit_end, name])
    entries.sort(key=lambda t: t[0])
    out = []
    for i, (start, expl_end, name) in enumerate(entries):
        if expl_end is not None:
            out.append((start, expl_end - 1, name, 'explicit'))
        elif i + 1 < len(entries):
            out.append((start, entries[i + 1][0] - 1, name, 'auto'))
        else:
            out.append((start, 0xFFFF, name, 'auto'))
    return out


def load_cfg_names(bank_hex: str):
    """Returns set of local-bank-addrs that have a `name` entry (i.e.
    already identified as sub-entries or call aliases)."""
    cfg_path = os.path.join(CFG_DIR, f'bank{bank_hex.lower()}.cfg')
    names = set()
    if not os.path.isfile(cfg_path):
        return names
    bank_int = int(bank_hex, 16)
    with open(cfg_path, 'r', encoding='utf-8') as f:
        for line in f:
            line = line.split('#', 1)[0].strip()
            if not line.startswith('name '):
                continue
            parts = line.split()
            if len(parts) < 3:
                continue
            try:
                full = int(parts[1], 16)
            except ValueError:
                continue
            if (full >> 16) == bank_int:
                names.add(full & 0xFFFF)
    return names


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    targets = sys.argv[1:]
    if targets == ['all']:
        targets = ['00', '01', '02', '03', '04', '05', '07', '0C', '0D']

    grand_total = 0
    for bank in targets:
        addressed, call_targets = scan_smwdisx_labels_and_calls(bank)
        if not addressed:
            continue
        funcs = load_cfg_funcs(bank)
        named = load_cfg_names(bank)
        func_starts = {f[0] for f in funcs}

        # Build a name → addr map for the addressed labels (for "is this
        # label a call target" lookup).
        name_to_addr = {n: a for a, n in addressed}

        candidates = []
        for start, end_incl, fname, kind in funcs:
            for laddr, lname in addressed:
                if laddr <= start or laddr > end_incl:
                    continue
                if laddr in func_starts or laddr in named:
                    continue
                is_call_target = lname in call_targets
                candidates.append((laddr, lname, fname, kind, is_call_target))

        if not candidates:
            print(f'# bank {bank}: no missing cfg entries')
            continue
        call_tgt_count = sum(1 for c in candidates if c[4])
        print(f'# bank {bank}: {len(candidates)} labels inside cfg func ranges '
              f'(of which {call_tgt_count} are JSR/JSL targets = likely missing func entries)')
        for laddr, lname, fname, kind, is_call in candidates:
            marker = ' [CALL TARGET]' if is_call else ''
            print(f'  ${bank}:{laddr:04X}  {lname}  inside {fname} ({kind}){marker}')
        grand_total += call_tgt_count
        print()

    print(f'# Total likely missing func entries: {grand_total}')


if __name__ == '__main__':
    main()
