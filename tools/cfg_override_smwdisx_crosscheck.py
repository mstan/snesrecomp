#!/usr/bin/env python3
"""
cfg_override_smwdisx_crosscheck — for each load-bearing cfg override,
cross-reference SMWDisX/bank_XX.asm to flag likely-wrong overrides.

For `end:X` overrides:
  - Find the function's start address (cfg_start) and its cfg_end.
  - In SMWDisX, look at instructions in [cfg_start, cfg_end) and right
    at cfg_end.
  - Classify:
      CLEAN   : cfg_end is exactly at a SMWDisX CODE_/ADDR_/named label
                AND the last insn before cfg_end is RTS/RTL/RTI (natural
                function boundary).
      OVERWIDE: cfg_end is past a natural terminator — there's an RTS/
                RTL at some X' < cfg_end and no code reaches past X'
                via branch (i.e. cfg_end includes dead bytes).
      SUSPECT : cfg_end lands mid-block (no terminator immediately
                before, and no label at cfg_end). Potentially wrong.
      UNCLEAR : cannot classify.

Output: markdown-ish report of every load-bearing end: in each bucket.
The SUSPECT / OVERWIDE flags are the review queue.

Usage:
    python snesrecomp/tools/cfg_override_smwdisx_crosscheck.py --type end
    python snesrecomp/tools/cfg_override_smwdisx_crosscheck.py --type end --bank 00
    python snesrecomp/tools/cfg_override_smwdisx_crosscheck.py --type end --list SUSPECT
"""
import argparse
import json
import pathlib
import re
import sys
from typing import Dict, List, Optional, Tuple

REPO = pathlib.Path(__file__).resolve().parent.parent
PARENT = pathlib.Path('F:/Projects/SuperMarioWorldRecomp')
SMWDISX_DIR = PARENT / 'SMWDisX'
RESULTS_DIR = REPO / 'tools' / 'cfg_audit_results'

LABEL_RE = re.compile(r'^([A-Za-z_][\w_]*):')
# "    RTS" / "    RTL" / "    RTI" (possibly with trailing comment)
RTS_RE = re.compile(r'^\s+(?:RT[SLI])\b')
# Address comment — SMWDisX doesn't emit absolute addrs in the listing
# itself, so we reconstruct them by tracking the file's state.


def parse_smwdisx_bank(bank: int) -> Dict:
    """Label→address map for SMWDisX bank `bank`.

    Source 1: `.sym` files — authoritative public symbols but NOT
    internal `CODE_XXXXXX` labels.
    Source 2: `bank_XX.asm` — scan for `CODE_BBXXXX:`, `ADDR_BBXXXX:`,
    `Return02XXXX:`, and other embedded-address labels to get the
    internal boundary anchors the sym file omits.
    """
    labels_by_addr: Dict[int, List[str]] = {}
    # 1. .sym files
    for sym_name in ('SMW_E0.sym', 'SMW_E1.sym'):
        sym_path = SMWDISX_DIR / sym_name
        if not sym_path.exists(): continue
        for line in sym_path.read_text(encoding='utf-8', errors='replace').splitlines():
            m = re.match(r'^([0-9A-Fa-f]{8})\s+(\S+)', line)
            if not m: continue
            full = int(m.group(1), 16)
            tgt_bank = (full >> 16) & 0xFF
            tgt_addr = full & 0xFFFF
            if tgt_bank != bank: continue
            labels_by_addr.setdefault(tgt_addr, []).append(m.group(2))
    # 2. bank_XX.asm — scan for embedded-address labels.
    asm_path = SMWDISX_DIR / f'bank_{bank:02x}.asm'
    if asm_path.exists():
        # Match `NAME_XXYYYY:` where XX is the bank (hex) and YYYY is
        # the 16-bit addr. Also bare labels like `Return02XXXX:` or
        # `LoadSpriteLoopStrt:` with no embedded addr (can't resolve —
        # skip those).
        bank_hex_up = f'{bank:02X}'
        bank_hex_lo = f'{bank:02x}'
        lbl_re = re.compile(
            rf'^([A-Za-z_][\w]*(?:{bank_hex_up}|{bank_hex_lo})([0-9A-Fa-f]{{4}}))\s*:'
        )
        for line in asm_path.read_text(encoding='utf-8', errors='replace').splitlines():
            m = lbl_re.match(line)
            if not m: continue
            name = m.group(1)
            try:
                addr = int(m.group(2), 16) & 0xFFFF
            except ValueError:
                continue
            labels_by_addr.setdefault(addr, []).append(name)
    return {'labels': labels_by_addr}


def classify_end(bank: int, cfg_start: int, cfg_end: int,
                  smwdisx_data: Dict,
                  d_end: Optional[int] = None,
                  cfg_func_addrs: Optional[set] = None) -> Tuple[str, str]:
    """Return (class, reason).

    If cfg_func_addrs is provided and cfg_end is one of the cfg's
    sibling-func start addrs, any "cfg_end < d_end" gap is not a
    truncation — the sibling covers the rest of the body.
    """
    labels = smwdisx_data.get('labels', {})
    # Tier 1: explicit SMWDisX label at cfg_end (or within 4 bytes)
    if cfg_end in labels:
        return ('CLEAN', f'SMWDisX label(s) at ${cfg_end:04x}: '
                         f'{",".join(labels[cfg_end][:3])}')
    before_addrs = sorted(a for a in labels if a < cfg_end and a > cfg_start)
    after_addrs = sorted(a for a in labels if a > cfg_end)
    nearest_before = before_addrs[-1] if before_addrs else None
    nearest_after = after_addrs[0] if after_addrs else None
    if nearest_after is not None and nearest_after - cfg_end <= 4:
        return ('CLEAN',
                f'cfg_end ${cfg_end:04x} within 4 bytes of SMWDisX label '
                f'${nearest_after:04x} ({",".join(labels[nearest_after][:2])})')
    if nearest_before is not None and cfg_end - nearest_before <= 4:
        return ('CLEAN',
                f'cfg_end ${cfg_end:04x} within 4 bytes after SMWDisX label '
                f'${nearest_before:04x} ({",".join(labels[nearest_before][:2])})')
    # Tier 2: compare vs discoverer's per-function d_end.
    # If cfg_end < d_end: cfg NARROWS the function — koopa-class suspicion.
    # BUT: if cfg_end is a sibling func's start, the sibling covers the
    # rest of the body; d_end was just walker crossing into sibling code
    # via natural fall-through. Not a truncation.
    # If cfg_end > d_end by a lot: cfg widens past natural body — fossil
    # or over-inclusion (may subsume sibling bytes).
    if d_end is not None:
        sibling_covers = (cfg_func_addrs is not None
                          and cfg_end in cfg_func_addrs)
        if cfg_end < d_end and not sibling_covers:
            return ('SUSPECT_NARROW',
                    f'cfg_end ${cfg_end:04x} < d_end ${d_end:04x} '
                    f'(may truncate real code; no sibling func at cfg_end)')
        if cfg_end > d_end + 0x20:
            return ('SUSPECT_WIDE',
                    f'cfg_end ${cfg_end:04x} >> d_end ${d_end:04x} '
                    f'(extends far past natural body)')
    return ('SUSPECT',
            f'cfg_end ${cfg_end:04x} has no SMWDisX label within 4 bytes. '
            f'Nearest: ${nearest_before:04x}' if nearest_before is not None
            else f'cfg_end ${cfg_end:04x}: no nearby SMWDisX label')


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument('--type', default='end')
    p.add_argument('--bank', type=lambda x: int(x, 16))
    p.add_argument('--list', choices=['CLEAN', 'SUSPECT', 'SUSPECT_NARROW',
                                       'SUSPECT_WIDE', 'UNCLEAR'])
    p.add_argument('--limit', type=int, default=0,
                   help='0 = no limit')
    args = p.parse_args()
    if args.type != 'end':
        print('Only --type end supported for now.', file=sys.stderr)
        return 1

    # Load latest validator results
    cands = sorted(RESULTS_DIR.glob(f'{args.type}_*.json'))
    if not cands:
        print('No validator results found. Run cfg_override_validator.py first.')
        return 1
    data = json.loads(cands[-1].read_text())
    results = data['results']
    # Filter to load-bearing
    load_bearing = [r for r in results if r.get('diff_line_count', -1) > 0]
    if args.bank is not None:
        load_bearing = [r for r in load_bearing if r['bank'] == args.bank]

    # Parse SMWDisX per bank
    smwdisx_by_bank: Dict[int, Dict] = {}
    for bank in sorted({r['bank'] for r in load_bearing}):
        print(f'Parsing SMWDisX bank {bank:02x}...', file=sys.stderr)
        smwdisx_by_bank[bank] = parse_smwdisx_bank(bank)
    # Compute discoverer d_end and cfg-func-addr set per bank.
    d_ends_by_bank: Dict[int, Dict[int, int]] = {}
    cfg_func_addrs_by_bank: Dict[int, set] = {}
    try:
        sys.path.insert(0, str(REPO / 'recompiler'))
        from snes65816 import load_rom  # noqa: E402
        import discover as _disc  # noqa: E402
        rom = load_rom(str(PARENT / 'smw.sfc'))
        import recomp as _recomp  # noqa: E402
        for bank in smwdisx_by_bank:
            cfg_path = PARENT / 'recomp' / f'bank{bank:02x}.cfg'
            if not cfg_path.exists(): continue
            cfg = _recomp.parse_config(str(cfg_path))
            seeds = {a for _, a, *_ in cfg.funcs}
            cfg_func_addrs_by_bank[bank] = seeds
            _, _, ends = _disc.discover_bank(
                rom, bank, external_seeds=seeds, return_ends=True)
            d_ends_by_bank[bank] = ends
    except Exception as e:
        print(f'  [!] d_end computation skipped: {e}', file=sys.stderr)

    # Classify
    classified = {'CLEAN': [], 'SUSPECT': [], 'SUSPECT_NARROW': [],
                  'SUSPECT_WIDE': [], 'UNCLEAR': []}
    for r in load_bearing:
        bank = r['bank']
        cfg_start = r['addr']
        try:
            cfg_end = int(r['token_value'], 16)
        except (ValueError, KeyError):
            classified['UNCLEAR'].append((r, 'cannot parse end value'))
            continue
        sd = smwdisx_by_bank.get(bank, {})
        d_end = d_ends_by_bank.get(bank, {}).get(cfg_start)
        cfg_addrs = cfg_func_addrs_by_bank.get(bank, set())
        cls, reason = classify_end(bank, cfg_start, cfg_end, sd, d_end, cfg_addrs)
        classified[cls].append((r, reason))

    # Summary
    total = sum(len(v) for v in classified.values())
    print(f'\n# SMWDisX cross-check for load-bearing {args.type} overrides '
          f'(total={total})')
    print(f'  CLEAN          (cfg_end near SMWDisX label)   : {len(classified["CLEAN"])}')
    print(f'  SUSPECT        (no label within 4 bytes)      : {len(classified["SUSPECT"])}')
    print(f'  SUSPECT_NARROW (cfg_end < discoverer d_end)   : {len(classified["SUSPECT_NARROW"])}  **koopa-shape candidates**')
    print(f'  SUSPECT_WIDE   (cfg_end >> discoverer d_end)  : {len(classified["SUSPECT_WIDE"])}  over-inclusive')
    print(f'  UNCLEAR                                        : {len(classified["UNCLEAR"])}')

    if args.list:
        to_show = classified[args.list]
        if args.limit:
            to_show = to_show[:args.limit]
        print(f'\n## {args.list} ({len(to_show)} shown)')
        print(f'{"bank":<5} {"addr":<6} {"name":<55} {"end:":>8}   reason')
        for r, reason in to_show:
            print(f'  {r["bank"]:02x}    {r["addr"]:04x}   '
                  f'{r["name"][:55]:<55} end:{r["token_value"]:<5}  {reason}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
