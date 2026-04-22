#!/usr/bin/env python3
"""
cfg_override_strip — apply the redundant classification from
cfg_override_validator results by actually removing those overrides
from the cfg files.

Only acts on overrides whose latest validator run classified as
diff_line_count == 0 (redundant). Never touches load-bearing overrides.

After apply, user should:
  1. Regen all banks.
  2. Rebuild Release|x64.
  3. Live-boot the exe and confirm no regression.
  4. Re-run `cfg_override_validator.py --type <type>` to rebase for next
     session (the cfg is now smaller; re-classification may shift as
     cross-override interactions settle).

Usage:
    python snesrecomp/tools/cfg_override_strip.py --type end --dry-run
    python snesrecomp/tools/cfg_override_strip.py --type end --apply
"""
import argparse
import json
import pathlib
import re
import sys
from typing import Dict, List, Tuple

REPO = pathlib.Path(__file__).resolve().parent.parent
PARENT = pathlib.Path('F:/Projects/SuperMarioWorldRecomp')
CFG_DIR = PARENT / 'recomp'
RESULTS_DIR = REPO / 'tools' / 'cfg_audit_results'


def latest_result_for(override_type: str) -> pathlib.Path:
    cands = sorted(RESULTS_DIR.glob(f'{override_type}_*.json'))
    if not cands:
        raise FileNotFoundError(f'No results for {override_type}')
    return cands[-1]


def strip_token_from_bank(bank: int, ops: List[Dict], dry_run: bool) -> int:
    """Apply strip ops (each {line_no, token}) to bank's cfg. Preserves
    line endings. Returns count actually stripped."""
    cfg_path = CFG_DIR / f'bank{bank:02x}.cfg'
    if not cfg_path.exists():
        return 0
    raw = cfg_path.read_bytes()
    nl = b'\r\n' if b'\r\n' in raw else b'\n'
    text = raw.decode('utf-8', errors='replace')
    lines = text.split(nl.decode('utf-8'))
    trailing = lines and lines[-1] == ''
    if trailing:
        lines = lines[:-1]
    # Apply each op — careful: ops must reference unique line_no values
    # from the same validator run so we don't double-edit.
    applied = 0
    for op in ops:
        ln = op['line_no']
        tok = op['token']
        if ln >= len(lines):
            continue
        tok_esc = re.escape(tok)
        new_line = re.sub(r'\s*' + tok_esc + r'(?!\S)', '', lines[ln], count=1)
        if new_line != lines[ln]:
            lines[ln] = new_line
            applied += 1
    if not dry_run and applied:
        out = nl.decode('utf-8').join(lines)
        if trailing:
            out += nl.decode('utf-8')
        cfg_path.write_bytes(out.encode('utf-8'))
    return applied


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument('--type', required=True)
    p.add_argument('--apply', action='store_true')
    p.add_argument('--dry-run', action='store_true')
    p.add_argument('--bank', type=lambda x: int(x, 16), help='limit to one bank')
    args = p.parse_args()

    dry = args.dry_run or not args.apply
    path = latest_result_for(args.type)
    data = json.loads(path.read_text())
    results = data['results']

    redundant = [r for r in results if r.get('diff_line_count', -1) == 0]
    if args.bank is not None:
        redundant = [r for r in redundant if r['bank'] == args.bank]
    by_bank: Dict[int, List[Dict]] = {}
    for r in redundant:
        by_bank.setdefault(r['bank'], []).append(r)

    print(f'Type: {args.type}  (source: {path.name})')
    print(f'Redundant overrides to strip: {len(redundant)}')
    for bank in sorted(by_bank):
        print(f'  bank {bank:02x}: {len(by_bank[bank])} overrides')

    if not args.apply:
        print('\nDRY RUN. Re-run with --apply to strip.')
        return 0

    total_applied = 0
    for bank, ops in sorted(by_bank.items()):
        # Sort descending by line_no — when multiple ops on same line,
        # applying later-offset first keeps earlier-offsets valid. For
        # single-token-per-line case (the norm), ordering is moot.
        ops_sorted = sorted(ops, key=lambda o: -o['line_no'])
        n = strip_token_from_bank(bank, ops_sorted, dry_run=False)
        print(f'  bank {bank:02x}: stripped {n}')
        total_applied += n
    print(f'\nTotal stripped: {total_applied}')
    print('\nNext steps:')
    print('  1. Regen all banks:')
    print('     for b in 00 01 02 03 04 05 07 0c 0d; do \\')
    print('       python snesrecomp/recompiler/recomp.py smw.sfc recomp/bank$b.cfg \\')
    print('         --reverse-debug -o src/gen/smw_${b}_gen.c; done')
    print('  2. Sync funcs.h:  python tools/sync_funcs_h.py')
    print('  3. Build Release|x64 and verify gen-C diff is empty.')
    print('  4. Re-run validator to rebase: python snesrecomp/tools/cfg_override_validator.py --type %s --all' % args.type)
    return 0


if __name__ == '__main__':
    sys.exit(main())
