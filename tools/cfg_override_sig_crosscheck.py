#!/usr/bin/env python3
"""
cfg_override_sig_crosscheck — compare each cfg sig: override against
what augment_cfg_sigs_from_livein would derive independently from ROM.

Classifies each load-bearing sig: override into:
  AGREES       : cfg sig is what live-in would produce (cfg==derived).
                 Strippable in principle.
  CFG_WIDER    : cfg declares params that live-in doesn't see (normal
                 — live-in is conservative; cfg is backup). Cross-
                 check: are the extras genuinely needed (callers use
                 them) or stale?
  CFG_NARROWER : cfg declares fewer params than live-in derives —
                 recompiler auto-adds, so cfg just loses credit.
  TYPE_DIFF    : same params, different types (uint8 vs uint16 etc).
                 Potentially wrong.
  RET_DIFF     : different return types.
  UNCLEAR      : couldn't compute live-in.

The CFG_WIDER and TYPE_DIFF buckets are the review candidates for
bugs like #8 (where an ABI override might be wrong).
"""
import argparse
import json
import pathlib
import re
import sys
from typing import Dict, List, Optional, Tuple

REPO = pathlib.Path(__file__).resolve().parent.parent
PARENT = pathlib.Path('F:/Projects/SuperMarioWorldRecomp')
RESULTS_DIR = REPO / 'tools' / 'cfg_audit_results'

sys.path.insert(0, str(REPO / 'recompiler'))
from snes65816 import load_rom  # noqa: E402
import recomp  # noqa: E402


def derive_livein_sig(rom, cfg, start_addr: int) -> Optional[str]:
    """Decode the function at start_addr using cfg's mode overrides and
    return what _augment_sig_with_livein would produce starting from
    a bare `void()` sig. Mirrors augment_cfg_sigs_from_livein's logic.
    """
    non_skip = [(f, a, s, e, mo, h) for f, a, s, e, mo, h in cfg.funcs if f not in cfg.skip]
    entry = None
    idx = -1
    for i, tup in enumerate(non_skip):
        if tup[1] == start_addr:
            entry = tup
            idx = i
            break
    if entry is None:
        return None
    fname, start, sig_tup, eovr, mo, _h = entry
    if eovr is not None:
        end_addr = eovr
    elif idx + 1 < len(non_skip):
        end_addr = non_skip[idx + 1][1]
    else:
        end_addr = 0x10000
    known_func_addrs = set(cfg.names.keys())
    for _fname, addr, *_ in cfg.funcs:
        known_func_addrs.add((cfg.bank << 16) | addr)
    try:
        insns = recomp.decode_func(
            rom, cfg.bank, start, end=end_addr,
            jsl_dispatch=cfg.jsl_dispatch or None,
            jsl_dispatch_long=cfg.jsl_dispatch_long or None,
            dispatch_known_addrs=known_func_addrs,
            mode_overrides=mo or None,
            exclude_ranges=cfg.exclude_ranges or None,
            known_func_starts=known_func_addrs,
            validate_branches=False)
    except Exception:
        return None
    if not insns:
        return None
    try:
        live_in = recomp.infer_live_in_regs(
            insns, start, bank=cfg.bank,
            callee_sigs=cfg.sigs,
            callee_clobbers=getattr(cfg, 'clobbers', None))
    except Exception:
        return None
    return recomp._augment_sig_with_livein('void()', live_in)


def parse_sig(sig: str) -> Tuple[str, List[Tuple[str, str]]]:
    if not sig:
        return ('void', [])
    m = re.match(r'(\w+)\s*\((.*)\)', sig)
    if not m:
        return (sig, [])
    ret = m.group(1)
    params_str = m.group(2).strip()
    params = []
    if params_str:
        for tok in params_str.split(','):
            tok = tok.strip()
            if '_' in tok:
                t, n = tok.rsplit('_', 1)
                params.append((t, n))
            else:
                params.append((tok, ''))
    return (ret, params)


def classify(cfg_sig: str, derived_sig: Optional[str]) -> Tuple[str, str]:
    if derived_sig is None:
        return ('UNCLEAR', 'could not compute live-in')
    cret, cparams = parse_sig(cfg_sig)
    dret, dparams = parse_sig(derived_sig)
    cparam_names = {n for _t, n in cparams}
    dparam_names = {n for _t, n in dparams}
    if cret != dret:
        return ('RET_DIFF', f'cfg ret={cret}, livein ret={dret}')
    if cparam_names == dparam_names:
        # check types
        ctypes = {n: t for t, n in cparams}
        dtypes = {n: t for t, n in dparams}
        type_diffs = [n for n in cparam_names if ctypes.get(n) != dtypes.get(n)]
        if type_diffs:
            return ('TYPE_DIFF',
                    f'params agree but types differ: '
                    f'{",".join(f"{n}:{ctypes[n]}!={dtypes[n]}" for n in type_diffs)}')
        return ('AGREES', 'cfg matches live-in exactly')
    extra_in_cfg = cparam_names - dparam_names
    extra_in_livein = dparam_names - cparam_names
    if extra_in_cfg and not extra_in_livein:
        return ('CFG_WIDER',
                f'cfg has extra params: {",".join(sorted(extra_in_cfg))}')
    if extra_in_livein and not extra_in_cfg:
        return ('CFG_NARROWER',
                f'live-in has extra params: {",".join(sorted(extra_in_livein))}')
    return ('CFG_WIDER',  # mixed: both sides differ; call it wider+review
            f'cfg has {",".join(sorted(extra_in_cfg))}, '
            f'live-in has {",".join(sorted(extra_in_livein))}')


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument('--bank', type=lambda x: int(x, 16))
    p.add_argument('--list', choices=['AGREES', 'CFG_WIDER', 'CFG_NARROWER',
                                       'TYPE_DIFF', 'RET_DIFF', 'UNCLEAR'])
    p.add_argument('--limit', type=int, default=0)
    args = p.parse_args()

    cands = sorted(RESULTS_DIR.glob('sig_*.json'))
    if not cands:
        print('No sig: validator results found.')
        return 1
    data = json.loads(cands[-1].read_text())
    load_bearing = [r for r in data['results'] if r.get('diff_line_count', -1) > 0]
    if args.bank is not None:
        load_bearing = [r for r in load_bearing if r['bank'] == args.bank]

    rom = load_rom(str(PARENT / 'smw.sfc'))
    cfg_cache: Dict[int, object] = {}
    def get_cfg(bank):
        if bank not in cfg_cache:
            cfg_cache[bank] = recomp.parse_config(str(PARENT / 'recomp' / f'bank{bank:02x}.cfg'))
        return cfg_cache[bank]

    classified = {k: [] for k in ['AGREES', 'CFG_WIDER', 'CFG_NARROWER',
                                    'TYPE_DIFF', 'RET_DIFF', 'UNCLEAR']}
    for r in load_bearing:
        bank = r['bank']
        cfg = get_cfg(bank)
        cfg_sig = r['token_value']
        derived = derive_livein_sig(rom, cfg, r['addr'])
        cls, reason = classify(cfg_sig, derived)
        classified[cls].append((r, derived, reason))

    total = sum(len(v) for v in classified.values())
    print(f'# sig: cross-check (load-bearing total={total})')
    for k, v in classified.items():
        print(f'  {k:<13} : {len(v)}')
    if args.list:
        to_show = classified[args.list]
        if args.limit:
            to_show = to_show[:args.limit]
        print(f'\n## {args.list} ({len(to_show)} shown)')
        print(f'{"bank":<5} {"addr":<6} {"name":<50} cfg_sig  -> derived  reason')
        for r, derived, reason in to_show:
            print(f'  {r["bank"]:02x}    {r["addr"]:04x}   {r["name"][:50]:<50} '
                  f'{r["token_value"]} -> {derived}  ({reason})')
    return 0


if __name__ == '__main__':
    sys.exit(main())
