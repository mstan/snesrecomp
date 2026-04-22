#!/usr/bin/env python3
"""
cfg_override_triage — consume cfg_override_validator JSON results and
emit human-readable classification reports.

Reports produced:
  - summary table per bank per override type
  - redundant list (candidates for automated strip)
  - load-bearing list (candidates for SMWDisX cross-check)
  - regen-failed list (investigate independently)

Usage:
    python snesrecomp/tools/cfg_override_triage.py --summary
    python snesrecomp/tools/cfg_override_triage.py --list redundant --type end
    python snesrecomp/tools/cfg_override_triage.py --list load-bearing --type end --limit 20
"""
import argparse
import json
import pathlib
import sys
from typing import Dict, List

REPO = pathlib.Path(__file__).resolve().parent.parent
RESULTS_DIR = REPO / 'tools' / 'cfg_audit_results'


def latest_result_for(override_type: str) -> pathlib.Path:
    candidates = sorted(RESULTS_DIR.glob(f'{override_type}_*.json'))
    if not candidates:
        raise FileNotFoundError(f'No results found for type {override_type} in {RESULTS_DIR}')
    return candidates[-1]


def cmd_summary(args) -> None:
    # Gather all override-type results present in RESULTS_DIR.
    types = sorted({p.name.split('_')[0] for p in RESULTS_DIR.glob('*.json')})
    if not types:
        print('No results found. Run cfg_override_validator.py first.')
        return
    for t in types:
        try:
            path = latest_result_for(t)
        except FileNotFoundError:
            continue
        data = json.loads(path.read_text())
        s = data['summary']
        print(f'\n=== {t}: total={s["total"]} '
              f'redundant={s["redundant"]} '
              f'load-bearing={s["load_bearing"]} '
              f'regen-failed={s["regen_failed"]} ===')
        print(f'  Source: {path.name}  ({s["timestamp"]})')
        print(f'  {"Bank":<6} {"Total":>6} {"Redund":>7} {"Load":>6}')
        for bank, b in sorted(s['by_bank'].items()):
            print(f'  {bank:<6} {b["total"]:>6} {b["redundant"]:>7} {b["load_bearing"]:>6}')


def cmd_list(args) -> None:
    path = latest_result_for(args.type)
    data = json.loads(path.read_text())
    results = data['results']
    if args.list == 'redundant':
        filt = [r for r in results if r['diff_line_count'] == 0]
    elif args.list == 'load-bearing':
        filt = [r for r in results if r['diff_line_count'] > 0]
        filt.sort(key=lambda r: r['diff_line_count'])
    elif args.list == 'regen-failed':
        filt = [r for r in results if not r.get('regen_ok', True)]
    else:
        filt = results
    if args.bank is not None:
        filt = [r for r in filt if r['bank'] == args.bank]
    if args.limit:
        filt = filt[:args.limit]
    print(f'# {args.list} {args.type} overrides ({len(filt)} shown)')
    print(f'# {"bank":<5} {"addr":<6} {"name":<50} {args.type}  diff_lines')
    for r in filt:
        print(f'  {r["bank"]:02x}    {r["addr"]:04x}   {r["name"]:<50} {r["token"]:<16} {r["diff_line_count"]}')


def main() -> int:
    p = argparse.ArgumentParser()
    sub = p.add_subparsers(dest='cmd', required=False)
    p.set_defaults(cmd='summary')
    p.add_argument('--summary', action='store_const', dest='cmd', const='summary')
    p.add_argument('--list', choices=['redundant', 'load-bearing', 'regen-failed'])
    p.add_argument('--type', default='end')
    p.add_argument('--bank', type=lambda x: int(x, 16))
    p.add_argument('--limit', type=int, default=30)
    args = p.parse_args()

    if args.list:
        cmd_list(args)
    else:
        cmd_summary(args)
    return 0


if __name__ == '__main__':
    sys.exit(main())
