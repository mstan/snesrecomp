#!/usr/bin/env python3
"""
cfg_override_mode_crosscheck — for each mode-hint override (rep/repx/sep),
dump SMWDisX context so human can verify the claimed M/X state.

Output is a table:
  bank | addr | hint | SMWDisX context lines above/at/below

The human reviews whether the claimed mode matches what SEP/REP in
SMWDisX says should be the state at `addr`.
"""
import json
import pathlib
import re
import sys
from typing import Dict, List

REPO = pathlib.Path(__file__).resolve().parent.parent
PARENT = pathlib.Path('F:/Projects/SuperMarioWorldRecomp')
SMWDISX_DIR = PARENT / 'SMWDisX'
RESULTS_DIR = REPO / 'tools' / 'cfg_audit_results'


def load_bank_asm(bank: int) -> List[str]:
    """Return raw lines of SMWDisX bank_XX.asm."""
    p = SMWDISX_DIR / f'bank_{bank:02x}.asm'
    if not p.exists():
        return []
    return p.read_text(encoding='utf-8', errors='replace').splitlines()


def find_label_line(asm_lines: List[str], bank: int, addr: int) -> int:
    """Return line number of the SMWDisX label at bank:addr, or -1."""
    bank_hex = f'{bank:02X}'
    # Match `NAME_BBAAAA:` where BBAAAA includes bank+addr.
    needle_up = f'{bank_hex}{addr:04X}'
    needle_lo = f'{bank:02x}{addr:04x}'
    for i, line in enumerate(asm_lines):
        if not line or ':' not in line[:40]:
            continue
        if needle_up in line or needle_lo in line:
            return i
    return -1


def dump_context(asm_lines: List[str], line_no: int, before: int = 12,
                  after: int = 3) -> str:
    if line_no < 0:
        return '    (no SMWDisX label found)'
    lo = max(0, line_no - before)
    hi = min(len(asm_lines), line_no + after + 1)
    out = []
    for i in range(lo, hi):
        marker = '>>>' if i == line_no else '   '
        out.append(f'  {marker} {asm_lines[i]}')
    return '\n'.join(out)


def report_type(override_type: str) -> None:
    path = RESULTS_DIR / f'{override_type}_2026_04_22.json'
    if not path.exists():
        print(f'No results for {override_type}')
        return
    data = json.loads(path.read_text())
    load_bearing = [r for r in data['results'] if r.get('diff_line_count', -1) > 0]
    print(f'\n=== {override_type} load-bearing ({len(load_bearing)}) ===')
    asm_cache: Dict[int, List[str]] = {}
    for r in load_bearing:
        bank = r['bank']
        if bank not in asm_cache:
            asm_cache[bank] = load_bank_asm(bank)
        asm = asm_cache[bank]
        # Mode overrides apply at the token's value addr (e.g. rep:8ae8
        # applies M=0 at $8ae8 exactly).
        try:
            tgt_addr = int(r['token_value'], 16)
        except ValueError:
            continue
        line_no = find_label_line(asm, bank, tgt_addr)
        print(f'\n  bank {bank:02x}  addr ${r["addr"]:04x}  {r["name"][:50]}')
        print(f'    {r["token"]} — applies at ${tgt_addr:04x}')
        print(dump_context(asm, line_no, before=8, after=2))


def main() -> int:
    import argparse
    p = argparse.ArgumentParser()
    p.add_argument('--type', required=True,
                   choices=['rep', 'repx', 'sep'])
    args = p.parse_args()
    report_type(args.type)
    return 0


if __name__ == '__main__':
    sys.exit(main())
