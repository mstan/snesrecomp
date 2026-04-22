#!/usr/bin/env python3
"""
cfg_override_validator — classify every cfg override by strip-and-diff.

For each override token on a cfg `func` or `name` line, the validator
produces a modified cfg with that token removed, regens the bank, and
diffs against a baseline gen-C. If the diff is empty, the override is
REDUNDANT (framework agrees without it). If the diff is non-empty, the
override is LOAD-BEARING (framework disagrees; human review needed to
decide if the override is correct or wrong).

Supported override types (first-pass scope):
  - end:X        — function end bound
  - sig:X        — calling convention
  - rep:X        — M=0 mode hint
  - repx:X       — X=0 mode hint
  - sep:X        — M=1,X=1 mode hint
  - init_y:X     — Y register init
  - init_carry:X — carry init
  - carry_ret    — function returns carry
  - ret_y        — function returns Y
  - restores_x:X — X restore hint
  - y_after:X    — Y after return
  - x_after:X    — X after return
  - no_autodiscover — (on `no_autodiscover ADDR` line, not a token)

Results written to
  snesrecomp/tools/cfg_audit_results/<type>_YYYY_MM_DD.json

Usage:
    python snesrecomp/tools/cfg_override_validator.py --type end --all
    python snesrecomp/tools/cfg_override_validator.py --type end --bank 02
    python snesrecomp/tools/cfg_override_validator.py --type sig --bank 00

The validator is session-safe: it operates on TEMP copies of cfgs and
generates to TEMP paths. The working cfgs and src/gen files are NEVER
modified. After the audit completes, the user can consult
cfg_override_triage.py to act on the results.
"""
import argparse
import datetime
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
from typing import Dict, List, Optional, Tuple

REPO = pathlib.Path(__file__).resolve().parent.parent
PARENT = pathlib.Path('F:/Projects/SuperMarioWorldRecomp')
CFG_DIR = PARENT / 'recomp'
GEN_DIR = PARENT / 'src' / 'gen'
ROM = PARENT / 'smw.sfc'
RESULTS_DIR = REPO / 'tools' / 'cfg_audit_results'
RECOMP_PY = REPO / 'recompiler' / 'recomp.py'
BANKS = (0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x07, 0x0c, 0x0d)

FUNC_RE = re.compile(
    r'^(?P<lead>\s*)func\s+(?P<name>\S+)\s+(?P<addr>[0-9a-fA-F]{4})(?P<rest>.*)$'
)
# Token patterns per override type.
#   value token: `end:XXXX`, `sig:...`, `rep:XXXX`, etc.
#   flag token : `carry_ret`, `ret_y`
TOKEN_SPECS = {
    'end':          ('value', r'end:[0-9a-fA-F]+'),
    'sig':          ('value', r'sig:\S+'),
    'rep':          ('value', r'rep:[0-9a-fA-F]+'),
    'repx':         ('value', r'repx:[0-9a-fA-F]+'),
    'sep':          ('value', r'sep:[0-9a-fA-F]+'),
    'init_y':       ('value', r'init_y:\S+'),
    'init_carry':   ('value', r'init_carry:\S+'),
    'carry_ret':    ('flag',  r'carry_ret\b'),
    'ret_y':        ('flag',  r'ret_y\b'),
    'restores_x':   ('value', r'restores_x:\S+'),
    'y_after':      ('value', r'y_after:\S+'),
    'x_after':      ('value', r'x_after:\S+'),
}


def bank_gen_path(bank: int) -> pathlib.Path:
    return GEN_DIR / f'smw_{bank:02x}_gen.c'


def bank_cfg_path(bank: int) -> pathlib.Path:
    return CFG_DIR / f'bank{bank:02x}.cfg'


def regen_bank_to(bank: int, cfg_path: pathlib.Path, out_path: pathlib.Path) -> bool:
    """Regen a bank from a specific cfg path to a specific gen-C path.

    Returns True on success, False on recomp.py failure.
    """
    r = subprocess.run(
        [sys.executable, str(RECOMP_PY),
         str(ROM), str(cfg_path),
         '--reverse-debug', '-o', str(out_path)],
        capture_output=True, text=True, timeout=60,
    )
    return r.returncode == 0


def diff_stats(path_a: pathlib.Path, path_b: pathlib.Path) -> Tuple[int, int]:
    """Return (line_diff_count, byte_diff_count) between two files.

    Uses plain text diff via Python; no shell invocation.
    """
    try:
        a = path_a.read_bytes()
        b = path_b.read_bytes()
    except FileNotFoundError:
        return (-1, -1)
    if a == b:
        return (0, 0)
    # Count line-level differences (symmetric).
    al = a.decode('utf-8', errors='replace').splitlines()
    bl = b.decode('utf-8', errors='replace').splitlines()
    sa = set(al); sb = set(bl)
    # Asymmetric differences; totals both sides.
    line_count = len(sa ^ sb)
    byte_count = abs(len(a) - len(b)) + sum(1 for x, y in zip(a, b) if x != y)
    return (line_count, byte_count)


def find_overrides(bank: int, override_type: str) -> List[Dict]:
    """Scan bank cfg for every occurrence of the given override. Returns
    list of dicts with line_no, line_text, addr, name, token_match_span."""
    kind, pat = TOKEN_SPECS[override_type]
    tok_re = re.compile(r'(?<!\S)(' + pat + r')(?!\S)')
    results = []
    cfg_path = bank_cfg_path(bank)
    if not cfg_path.exists():
        return results
    text = cfg_path.read_text(encoding='utf-8', errors='replace')
    lines = text.splitlines(keepends=True)
    for i, raw in enumerate(lines):
        line = raw.rstrip('\r\n')
        m = FUNC_RE.match(line)
        if not m:
            continue
        try:
            addr = int(m.group('addr'), 16) & 0xFFFF
        except ValueError:
            continue
        fname = m.group('name')
        rest = m.group('rest')
        # Cap search to pre-comment body only.
        body_end = rest.find('#')
        if body_end < 0:
            body_end = len(rest)
        body = rest[:body_end]
        for tm in tok_re.finditer(body):
            tok = tm.group(0)
            # Extract value for value-tokens.
            if kind == 'value':
                val = tok.split(':', 1)[1]
            else:
                val = ''
            results.append({
                'bank': bank,
                'addr': addr,
                'name': fname,
                'line_no': i,
                'line_text': raw.rstrip('\n'),
                'token': tok,
                'token_value': val,
                'override_type': override_type,
            })
    return results


def build_cfg_without_override(bank: int, line_no: int, token: str) -> str:
    """Return cfg text with the specific token removed from line_no.

    Preserves all other content including line endings."""
    cfg_path = bank_cfg_path(bank)
    raw = cfg_path.read_bytes()
    nl = b'\r\n' if b'\r\n' in raw else b'\n'
    lines = raw.decode('utf-8', errors='replace').split(nl.decode('utf-8'))
    if line_no >= len(lines):
        return raw.decode('utf-8', errors='replace')
    line = lines[line_no]
    # Remove the token. Preserve surrounding whitespace conservatively —
    # collapse any leading whitespace of the token (the decoder tolerates
    # double-space).
    tok_esc = re.escape(token)
    new_line = re.sub(r'\s*' + tok_esc + r'(?!\S)', '', line, count=1)
    lines[line_no] = new_line
    return nl.decode('utf-8').join(lines)


def mirror_repo_layout(tmp_dir: pathlib.Path) -> pathlib.Path:
    """Create a mirrored recomp/ + src/funcs.h layout under tmp_dir so regens
    from inside tmp_dir resolve sibling cfgs and funcs.h the same way the
    baseline regen does.

    Returns the path to the mirrored recomp/ dir (containing all bank*.cfg
    copies). Callers modify one cfg at a time in-place for each test.
    """
    mirror_recomp = tmp_dir / 'recomp'
    mirror_src = tmp_dir / 'src'
    mirror_recomp.mkdir(parents=True, exist_ok=True)
    mirror_src.mkdir(parents=True, exist_ok=True)
    # Copy every bank*.cfg from the real recomp dir.
    for p in sorted(CFG_DIR.glob('bank*.cfg')):
        shutil.copy2(p, mirror_recomp / p.name)
    # Copy funcs.h so recomp.py's cfg_dir-relative search (`../src/funcs.h`)
    # resolves inside the mirror. Prefer the project's canonical copy at
    # PARENT/src/funcs.h; fall back to snesrecomp/../src/funcs.h if not
    # present (sync_funcs_h.py keeps both consistent).
    src_funcs_h = PARENT / 'src' / 'funcs.h'
    if not src_funcs_h.exists():
        src_funcs_h = REPO.parent / 'src' / 'funcs.h'
    if src_funcs_h.exists():
        shutil.copy2(src_funcs_h, mirror_src / 'funcs.h')
    return mirror_recomp


def mirror_cfg_path(mirror_recomp: pathlib.Path, bank: int) -> pathlib.Path:
    return mirror_recomp / f'bank{bank:02x}.cfg'


def validate_override(bank: int, override: Dict, baseline_gen: pathlib.Path,
                       tmp_dir: pathlib.Path,
                       mirror_recomp: pathlib.Path) -> Dict:
    """Strip a single override in the mirrored cfg, regen, diff, restore.

    The cfg modification happens IN-PLACE inside the mirrored recomp/ dir
    so recomp.py's sibling-cfg and funcs.h resolution behaves identically
    to the baseline regen. The original (baseline) cfg is restored after
    the test so subsequent tests start from a clean mirror.
    """
    mirror_cfg = mirror_cfg_path(mirror_recomp, bank)
    original_cfg_text = mirror_cfg.read_text(encoding='utf-8')
    tmp_gen = tmp_dir / f'smw_{bank:02x}_test.c'
    new_cfg_text = build_cfg_without_override(bank, override['line_no'], override['token'])
    try:
        mirror_cfg.write_text(new_cfg_text, encoding='utf-8')
        ok = regen_bank_to(bank, mirror_cfg, tmp_gen)
        if not ok:
            return dict(override, diff_line_count=-1, diff_byte_count=-1,
                        regen_ok=False)
        line_count, byte_count = diff_stats(tmp_gen, baseline_gen)
        return dict(override, diff_line_count=line_count, diff_byte_count=byte_count,
                    regen_ok=True)
    finally:
        mirror_cfg.write_text(original_cfg_text, encoding='utf-8')


def build_baselines(banks: List[int], baseline_dir: pathlib.Path,
                     mirror_recomp: pathlib.Path) -> Dict[int, pathlib.Path]:
    """Regen each bank FROM the mirrored cfg layout into baseline_dir.

    Uses the mirror (not CFG_DIR) so baseline and per-override regens share
    identical path-resolution context — same sibling cfgs, same funcs.h
    search result. Otherwise every stripped regen shows a path-induced
    diff unrelated to the override being tested.
    """
    out = {}
    for bank in banks:
        baseline_path = baseline_dir / f'smw_{bank:02x}_baseline.c'
        cfg_path = mirror_cfg_path(mirror_recomp, bank)
        ok = regen_bank_to(bank, cfg_path, baseline_path)
        if not ok:
            print(f'  [!] baseline regen failed for bank {bank:02x}', file=sys.stderr)
        out[bank] = baseline_path
    return out


def run_audit(banks: List[int], override_type: str, out_json: pathlib.Path) -> None:
    print(f'Auditing {override_type} overrides across banks: '
          f'{", ".join(f"{b:02x}" for b in banks)}', flush=True)

    with tempfile.TemporaryDirectory(prefix='cfgaudit_') as tmp_str:
        tmp_dir = pathlib.Path(tmp_str)
        baseline_dir = tmp_dir / 'baseline'
        baseline_dir.mkdir()

        mirror_recomp = mirror_repo_layout(tmp_dir)

        print('Building baseline gen-C per bank (from mirrored cfg layout)...', flush=True)
        baselines = build_baselines(banks, baseline_dir, mirror_recomp)

        all_results: List[Dict] = []
        for bank in banks:
            overrides = find_overrides(bank, override_type)
            print(f'  bank {bank:02x}: {len(overrides)} {override_type} overrides', flush=True)
            for idx, ov in enumerate(overrides):
                result = validate_override(bank, ov, baselines[bank], tmp_dir,
                                           mirror_recomp)
                all_results.append(result)
                if idx % 25 == 0 and idx > 0:
                    print(f'    ...bank {bank:02x} {idx}/{len(overrides)}', flush=True)

        # Summary counts.
        redundant = sum(1 for r in all_results if r['diff_line_count'] == 0)
        load_bearing = sum(1 for r in all_results if r['diff_line_count'] > 0)
        failed = sum(1 for r in all_results if not r.get('regen_ok', True))
        total = len(all_results)
        summary = {
            'override_type': override_type,
            'total': total,
            'redundant': redundant,
            'load_bearing': load_bearing,
            'regen_failed': failed,
            'by_bank': {},
            'timestamp': datetime.datetime.now().isoformat(),
        }
        for bank in banks:
            bres = [r for r in all_results if r['bank'] == bank]
            summary['by_bank'][f'{bank:02x}'] = {
                'total': len(bres),
                'redundant': sum(1 for r in bres if r['diff_line_count'] == 0),
                'load_bearing': sum(1 for r in bres if r['diff_line_count'] > 0),
            }

        RESULTS_DIR.mkdir(parents=True, exist_ok=True)
        out_json.write_text(json.dumps({
            'summary': summary,
            'results': all_results,
        }, indent=2))
        print(f'\nResults: {total} overrides'
              f' | redundant: {redundant}'
              f' | load-bearing: {load_bearing}'
              f' | regen-failed: {failed}')
        print(f'Written: {out_json}')


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument('--type', required=True, choices=list(TOKEN_SPECS.keys()))
    p.add_argument('--bank', type=lambda x: int(x, 16))
    p.add_argument('--all', action='store_true')
    p.add_argument('--out', type=pathlib.Path)
    args = p.parse_args()

    if not args.bank and not args.all:
        p.error('Specify --bank XX or --all')
    banks = [args.bank] if args.bank is not None else list(BANKS)

    if args.out is None:
        date = datetime.datetime.now().strftime('%Y_%m_%d')
        args.out = RESULTS_DIR / f'{args.type}_{date}.json'

    run_audit(banks, args.type, args.out)
    return 0


if __name__ == '__main__':
    sys.exit(main())
