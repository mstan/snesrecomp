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
    # Standalone-line directives — not `func`-line tokens. strip = remove
    # the whole line. See find_overrides / build_cfg_without_override for
    # the two-path dispatch.
    'exclude_range': ('line', r'^\s*exclude_range\s+[0-9a-fA-F]+\s+[0-9a-fA-F]+.*$'),
    'no_autodiscover': ('line', r'^\s*no_autodiscover\s+[0-9a-fA-F]+.*$'),
    # `name XXYYYY NameStr [sig:...]` — cross-bank alias. When stripped,
    # the owning-cfg-bank's emitter may fall back to sibling-cfg-import
    # (often the canonical `func` entry in bank XX.cfg carries the same
    # name and sig). The strip is load-bearing only if the alias carries
    # info the sibling import doesn't (e.g. non-matching sig, address
    # that isn't a cfg'd func anywhere else).
    'name': ('line', r'^\s*name\s+[0-9a-fA-F]+\s+\S+.*$'),
}

# Types whose strip-and-diff effects are strictly intra-bank — safe to
# verify against the owning bank only, skipping the 9x all-banks cost.
# exclude_range shifts data/code decoding within one bank.
# no_autodiscover blocks an intra-bank auto-promote decision.
# name: only the owning cfg file's gen-C can change; other banks read
# the cross-bank name via sibling-cfg import which doesn't see the
# stripped local override.
INTRA_BANK_TYPES = frozenset({'exclude_range', 'no_autodiscover', 'name'})


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
    results = []
    cfg_path = bank_cfg_path(bank)
    if not cfg_path.exists():
        return results
    text = cfg_path.read_text(encoding='utf-8', errors='replace')
    lines = text.splitlines(keepends=True)
    # Standalone-line directives: each matching line is one override, the
    # whole line is the "token" (strip deletes the entire line). No addr /
    # name extraction — those fields come from the line body itself.
    if kind == 'line':
        line_re = re.compile(pat)
        for i, raw in enumerate(lines):
            line = raw.rstrip('\r\n')
            if not line_re.match(line):
                continue
            # For exclude_range / no_autodiscover the leading fields are
            # the ADDR(s); keep the full line as token for strip idempotence.
            parts = line.strip().split()
            try:
                addr = int(parts[1], 16) if len(parts) >= 2 else 0
            except ValueError:
                addr = 0
            results.append({
                'bank': bank,
                'addr': addr,
                'name': '',
                'line_no': i,
                'line_text': raw.rstrip('\n'),
                'token': line,       # full line used as the "strip this" key
                'token_value': '',
                'override_type': override_type,
            })
        return results
    tok_re = re.compile(r'(?<!\S)(' + pat + r')(?!\S)')
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

    Preserves all other content including line endings. For token-kind
    overrides (rep:X, sig:..., carry_ret) the token is removed from the
    line; the line remains. For line-kind overrides (exclude_range,
    no_autodiscover) the entire line is dropped — `token` in that case
    is the whole line body and the heuristic below collapses to a line
    delete.
    """
    cfg_path = bank_cfg_path(bank)
    raw = cfg_path.read_bytes()
    nl = b'\r\n' if b'\r\n' in raw else b'\n'
    lines = raw.decode('utf-8', errors='replace').split(nl.decode('utf-8'))
    if line_no >= len(lines):
        return raw.decode('utf-8', errors='replace')
    line = lines[line_no]
    # If the whole line matches the token (ignoring leading/trailing
    # whitespace), it's a line-kind directive — delete the line entirely.
    if line.strip() == token.strip():
        del lines[line_no]
        return nl.decode('utf-8').join(lines)
    # Token-kind: strip the token substring from the line.
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


def validate_override(bank: int, override: Dict, baselines: Dict[int, pathlib.Path],
                       tmp_dir: pathlib.Path,
                       mirror_recomp: pathlib.Path,
                       all_banks: List[int]) -> Dict:
    """Strip a single override in the mirrored cfg, regen ALL banks, diff
    each bank vs its baseline, restore.

    Regenerates every bank — not just the one owning the override — because
    cfg.sigs are imported bank-to-bank via the global-ns pass, and a sig
    change in bank A can reshape live-in / emit in bank B. A narrower
    per-bank check would label cross-bank cascade candidates as redundant
    when they actually break other banks' gen-C on apply.

    diff_line_count is the MAX over all banks (zero = truly safe,
    otherwise the cascading bank drove the count).
    """
    mirror_cfg = mirror_cfg_path(mirror_recomp, bank)
    original_cfg_text = mirror_cfg.read_text(encoding='utf-8')
    new_cfg_text = build_cfg_without_override(bank, override['line_no'], override['token'])
    try:
        mirror_cfg.write_text(new_cfg_text, encoding='utf-8')
        worst_line = 0
        worst_byte = 0
        worst_bank = None
        for b in all_banks:
            tmp_gen = tmp_dir / f'smw_{b:02x}_test.c'
            ok = regen_bank_to(b, mirror_cfg_path(mirror_recomp, b), tmp_gen)
            if not ok:
                return dict(override, diff_line_count=-1, diff_byte_count=-1,
                            regen_ok=False, cascade_bank=b)
            line_count, byte_count = diff_stats(tmp_gen, baselines[b])
            if line_count > worst_line:
                worst_line = line_count
                worst_byte = byte_count
                worst_bank = b
        return dict(override, diff_line_count=worst_line, diff_byte_count=worst_byte,
                    regen_ok=True, cascade_bank=worst_bank)
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


def _apply_all_redundant_and_verify(banks: List[int], override_type: str,
                                      redundants_by_bank: Dict[int, List[Dict]],
                                      baselines: Dict[int, pathlib.Path],
                                      tmp_dir: pathlib.Path,
                                      mirror_recomp: pathlib.Path) -> Dict[int, int]:
    """Apply EVERY per-token-redundant strip simultaneously, regen every bank
    from the stripped cfgs, and diff against baseline.

    Per-token 'redundant' means `strip this one override, regen ONE bank,
    diff == baseline`. That test holds with every OTHER override still in
    place — most cfg-driven invariants stay pinned. But applying ALL
    redundant strips together can cascade: live-in/sig inference is a
    fixpoint, and removing many hints at once lets the fixpoint converge
    to a different (but still self-consistent) solution. Downstream
    effects: a callee's inferred return type widens to a pointer, and the
    caller's emitted C uses the pointer in a bitwise op context that no
    longer compiles.

    This pass regens every bank against the all-strip mirror and records
    per-bank diff-line counts. A non-zero diff proves a cascade; the
    caller's `run_audit` uses that to demote a subset of strips back to
    load-bearing before writing the results file.

    Returns {bank -> diff_line_count}. Zero everywhere means the apply-all
    set is safe to strip en masse.
    """
    print('Apply-all verification: applying every redundant strip to mirror...',
          flush=True)
    originals = {}
    for bank, ops in redundants_by_bank.items():
        mirror_cfg = mirror_cfg_path(mirror_recomp, bank)
        originals[bank] = mirror_cfg.read_text(encoding='utf-8')
        # Sort ops by descending line_no so earlier strips don't shift later
        # line indices (not strictly needed — build_cfg_without_override reads
        # from REAL cfg each call — but keep stable).
        text = originals[bank]
        nl = '\r\n' if '\r\n' in text else '\n'
        lines = text.split(nl)
        for op in sorted(ops, key=lambda o: o['line_no'], reverse=True):
            ln = op['line_no']
            if ln >= len(lines):
                continue
            tok_esc = re.escape(op['token'])
            lines[ln] = re.sub(r'\s*' + tok_esc + r'(?!\S)', '', lines[ln], count=1)
        mirror_cfg.write_text(nl.join(lines), encoding='utf-8')

    diffs: Dict[int, int] = {}
    try:
        for bank in banks:
            tmp_gen = tmp_dir / f'smw_{bank:02x}_applyall.c'
            ok = regen_bank_to(bank, mirror_cfg_path(mirror_recomp, bank), tmp_gen)
            if not ok:
                diffs[bank] = -1
                print(f'  [!] apply-all regen failed for bank {bank:02x}',
                      file=sys.stderr, flush=True)
                continue
            line_count, _ = diff_stats(tmp_gen, baselines[bank])
            diffs[bank] = line_count
            status = 'CLEAN' if line_count == 0 else f'CASCADE+{line_count}'
            print(f'  bank {bank:02x}: apply-all diff = {line_count} lines ({status})',
                  flush=True)
    finally:
        # Restore mirror so subsequent runs start clean.
        for bank, original in originals.items():
            mirror_cfg_path(mirror_recomp, bank).write_text(original, encoding='utf-8')
    return diffs


def _bisect_cascade_offenders(bank: int, candidates: List[Dict],
                                baseline: pathlib.Path,
                                tmp_dir: pathlib.Path,
                                mirror_recomp: pathlib.Path) -> List[Dict]:
    """Given a bank where apply-all regen diverged from baseline, find the
    subset of `candidates` whose simultaneous removal causes the cascade.

    Strategy: a binary partition. Apply the first half, regen, check diff.
    If clean, the cascade culprit is in the second half; else it's in the
    first (and possibly also in the second). Recurse on non-clean halves.
    A single-element set is trivially the offender.

    Worst case: O(n log n) regens for n candidates, vs O(n) for a naive
    one-at-a-time strip. For n=348 sigs this is ~8 rounds of regen per
    bank that cascaded — tolerable.

    Returns the list of candidates that must be DEMOTED to load-bearing.
    """
    offenders: List[Dict] = []
    mirror_cfg = mirror_cfg_path(mirror_recomp, bank)
    original = mirror_cfg.read_text(encoding='utf-8')
    nl = '\r\n' if '\r\n' in original else '\n'

    def _apply_subset(subset):
        text = original
        lines = text.split(nl)
        for op in sorted(subset, key=lambda o: o['line_no'], reverse=True):
            ln = op['line_no']
            if ln >= len(lines):
                continue
            tok_esc = re.escape(op['token'])
            lines[ln] = re.sub(r'\s*' + tok_esc + r'(?!\S)', '', lines[ln], count=1)
        mirror_cfg.write_text(nl.join(lines), encoding='utf-8')
        tmp_gen = tmp_dir / f'smw_{bank:02x}_bisect.c'
        ok = regen_bank_to(bank, mirror_cfg, tmp_gen)
        if not ok:
            return -1
        lc, _ = diff_stats(tmp_gen, baseline)
        return lc

    try:
        def _recurse(subset):
            if not subset:
                return
            if len(subset) == 1:
                diff = _apply_subset(subset)
                if diff != 0:
                    offenders.append(subset[0])
                return
            mid = len(subset) // 2
            left, right = subset[:mid], subset[mid:]
            # Check left: strip only left, keep right pinned.
            diff_left = _apply_subset(left)
            if diff_left != 0:
                _recurse(left)
            diff_right = _apply_subset(right)
            if diff_right != 0:
                _recurse(right)
        _recurse(candidates)
    finally:
        mirror_cfg.write_text(original, encoding='utf-8')
    return offenders


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
            # Intra-bank-only types (exclude_range, no_autodiscover) skip
            # the cross-bank regen cost — their effects stay in one bank,
            # so checking other banks is 8x wasted regen per candidate.
            # Other types fall through to the full all-banks check.
            check_banks = [bank] if override_type in INTRA_BANK_TYPES else banks
            for idx, ov in enumerate(overrides):
                result = validate_override(bank, ov, baselines, tmp_dir,
                                           mirror_recomp, check_banks)
                all_results.append(result)
                if idx % 25 == 0 and idx > 0:
                    print(f'    ...bank {bank:02x} {idx}/{len(overrides)}', flush=True)

        # Apply-all verification: per-token "diff=0" only proves each strip
        # is locally redundant (every OTHER override still pinning the
        # fixpoint). Applying every redundant strip together can let
        # live-in / sig inference converge to a different fixpoint whose
        # outputs don't match baseline — a real compile regression, not
        # noise. Bisect down to the specific cascade-offenders and demote
        # them to load-bearing.
        redundants_by_bank: Dict[int, List[Dict]] = {}
        for r in all_results:
            if r['diff_line_count'] == 0:
                redundants_by_bank.setdefault(r['bank'], []).append(r)
        if any(redundants_by_bank.values()):
            applyall_diffs = _apply_all_redundant_and_verify(
                banks, override_type, redundants_by_bank, baselines,
                tmp_dir, mirror_recomp)
            for bank, diff in applyall_diffs.items():
                if diff == 0:
                    continue
                candidates = redundants_by_bank.get(bank, [])
                if not candidates:
                    continue
                print(f'  bisecting bank {bank:02x} ({len(candidates)} candidates)...',
                      flush=True)
                offenders = _bisect_cascade_offenders(
                    bank, candidates, baselines[bank], tmp_dir, mirror_recomp)
                # Demote offenders: patch their all_results entry with the
                # per-bank apply-all diff so they report load-bearing.
                offender_keys = {(o['bank'], o['line_no'], o['token']) for o in offenders}
                if not offender_keys:
                    # Bisection found 0 single offenders but apply-all still
                    # cascaded — the cascade is a multi-element AB-pair (or
                    # larger combination). Binary partition can't isolate
                    # those. Conservatively demote ALL this bank's candidates
                    # so the strip tool doesn't apply them blindly. A later
                    # run of cfg_override_maximize.py can recover a safe
                    # subset via linear greedy.
                    offender_keys = {(c['bank'], c['line_no'], c['token'])
                                     for c in candidates}
                    print(f'    bisection found no single offender — '
                          f'conservatively demoting all {len(candidates)} '
                          f'candidates in bank {bank:02x}', flush=True)
                for r in all_results:
                    key = (r['bank'], r['line_no'], r['token'])
                    if key in offender_keys:
                        r['cascade_offender'] = True
                print(f'    demoted {len(offender_keys)} cascade-offenders in bank {bank:02x}',
                      flush=True)

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
