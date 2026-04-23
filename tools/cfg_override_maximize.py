#!/usr/bin/env python3
"""cfg_override_maximize — find the MAXIMAL safe-to-strip subset.

The basic validator records per-token empty-diff as "redundant". The
apply-all + bisection layer catches cascade offenders via binary
partition. But binary partition misses multi-element cascades where
no single candidate is individually at fault (AB-pair cascade). When
that happens the validator conservatively demotes the entire bank.

This tool recovers the maximum safe subset with a chunked-greedy
algorithm: walk candidates, admit a candidate only if adding it
doesn't cascade the global regen. O(n) regens worst-case, typically
closer to n/chunk_size + small tail.

Algorithm (chunk_size=16 default):
  keep = []
  for chunk in chunks_of(candidates):
    trial = keep + chunk
    regen all banks from mirror(trial stripped)
    if diff-vs-baseline == 0 for all banks:
      keep.extend(chunk)    # whole chunk admitted
    else:
      # Binary search within chunk.
      for c in chunk:
        trial2 = keep + [c]
        if regen-all-clean(trial2):
          keep.append(c)

Output: patches the latest audit JSON for the given override type so
that cascade_offender stays True on non-maximal candidates and flips
False on the maximal-safe subset. cfg_override_strip.py then applies
them.

Usage:
    python cfg_override_maximize.py --type sig
    python cfg_override_maximize.py --type end --chunk-size 8
"""
import argparse
import json
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
from typing import Dict, List, Tuple

REPO = pathlib.Path(__file__).resolve().parent.parent
PARENT = pathlib.Path('F:/Projects/SuperMarioWorldRecomp')
CFG_DIR = PARENT / 'recomp'
ROM = PARENT / 'smw.sfc'
RESULTS_DIR = REPO / 'tools' / 'cfg_audit_results'
RECOMP_PY = REPO / 'recompiler' / 'recomp.py'
BANKS = (0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x07, 0x0c, 0x0d)


def mirror_repo_layout(tmp_dir: pathlib.Path) -> pathlib.Path:
    mirror_recomp = tmp_dir / 'recomp'
    mirror_src = tmp_dir / 'src'
    mirror_recomp.mkdir(parents=True, exist_ok=True)
    mirror_src.mkdir(parents=True, exist_ok=True)
    for p in sorted(CFG_DIR.glob('bank*.cfg')):
        shutil.copy2(p, mirror_recomp / p.name)
    src_funcs_h = PARENT / 'src' / 'funcs.h'
    if src_funcs_h.exists():
        shutil.copy2(src_funcs_h, mirror_src / 'funcs.h')
    return mirror_recomp


def regen_bank(bank: int, cfg_path: pathlib.Path, out_path: pathlib.Path) -> bool:
    r = subprocess.run(
        [sys.executable, str(RECOMP_PY), str(ROM), str(cfg_path),
         '--reverse-debug', '-o', str(out_path)],
        capture_output=True, text=True, timeout=120,
    )
    return r.returncode == 0


def diff_clean(a: pathlib.Path, b: pathlib.Path) -> bool:
    return a.read_bytes() == b.read_bytes()


def apply_strips_to_mirror(mirror_recomp: pathlib.Path,
                            originals: Dict[int, str],
                            ops: List[Dict]) -> None:
    """Write each bank's mirror cfg with the given ops stripped.

    Starts from `originals` each time so ops composition is deterministic.
    """
    by_bank: Dict[int, List[Dict]] = {}
    for op in ops:
        by_bank.setdefault(op['bank'], []).append(op)
    for bank, text in originals.items():
        mirror_cfg = mirror_recomp / f'bank{bank:02x}.cfg'
        if bank not in by_bank:
            mirror_cfg.write_text(text, encoding='utf-8')
            continue
        nl = '\r\n' if '\r\n' in text else '\n'
        lines = text.split(nl)
        for op in sorted(by_bank[bank], key=lambda o: o['line_no'], reverse=True):
            ln = op['line_no']
            if ln >= len(lines):
                continue
            tok_esc = re.escape(op['token'])
            lines[ln] = re.sub(r'\s*' + tok_esc + r'(?!\S)', '', lines[ln], count=1)
        mirror_cfg.write_text(nl.join(lines), encoding='utf-8')


def regen_all_and_check(mirror_recomp: pathlib.Path, baseline_dir: pathlib.Path,
                         tmp_dir: pathlib.Path) -> bool:
    """Regen all banks from current mirror state. Return True if every
    bank matches baseline (byte-for-byte)."""
    for bank in BANKS:
        cfg = mirror_recomp / f'bank{bank:02x}.cfg'
        out = tmp_dir / f'smw_{bank:02x}_test.c'
        if not regen_bank(bank, cfg, out):
            print(f'    regen fail bank {bank:02x}', flush=True)
            return False
        if not diff_clean(out, baseline_dir / f'smw_{bank:02x}_baseline.c'):
            return False
    return True


def chunked_greedy(candidates: List[Dict], mirror_recomp: pathlib.Path,
                    baseline_dir: pathlib.Path, tmp_dir: pathlib.Path,
                    originals: Dict[int, str], chunk_size: int = 16) -> List[Dict]:
    """Return the maximum subset of candidates whose simultaneous strip
    still produces byte-identical gen-C in every bank."""
    keep: List[Dict] = []
    n = len(candidates)
    i = 0
    admitted = 0
    while i < n:
        chunk = candidates[i:i + chunk_size]
        trial = keep + chunk
        apply_strips_to_mirror(mirror_recomp, originals, trial)
        if regen_all_and_check(mirror_recomp, baseline_dir, tmp_dir):
            keep.extend(chunk)
            admitted += len(chunk)
            print(f'  chunk {i}-{i+len(chunk)-1}: all {len(chunk)} admitted '
                  f'(keep={len(keep)}/{n} = {len(keep)*100//n}%)', flush=True)
        else:
            # Chunk rejects — test each individually on top of keep.
            for c in chunk:
                trial2 = keep + [c]
                apply_strips_to_mirror(mirror_recomp, originals, trial2)
                if regen_all_and_check(mirror_recomp, baseline_dir, tmp_dir):
                    keep.append(c)
                    admitted += 1
            print(f'  chunk {i}-{i+len(chunk)-1}: partial — '
                  f'{sum(1 for c in chunk if c in keep)}/{len(chunk)} admitted '
                  f'(keep={len(keep)}/{n} = {len(keep)*100//n}%)', flush=True)
        i += chunk_size
    # Restore mirror to pristine before returning so caller's state is clean.
    apply_strips_to_mirror(mirror_recomp, originals, [])
    return keep


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--type', required=True)
    ap.add_argument('--chunk-size', type=int, default=16)
    ap.add_argument('--bank', type=lambda x: int(x, 16),
                    help='Only maximize within one bank')
    args = ap.parse_args()

    # Load latest audit results.
    cands_path = sorted(RESULTS_DIR.glob(f'{args.type}_*.json'))[-1]
    data = json.loads(cands_path.read_text())
    # Candidates: per-token-redundant (diff=0) that were demoted via
    # cascade_offender=True. The purpose is to try each of those again
    # on top of an evolving "safe" set.
    cand_pool = [r for r in data['results']
                 if r.get('diff_line_count') == 0
                 and r.get('cascade_offender') is True]
    if args.bank is not None:
        cand_pool = [r for r in cand_pool if r['bank'] == args.bank]
    if not cand_pool:
        print(f'No cascade-demoted candidates for type {args.type} '
              f'(bank={args.bank!r}). Nothing to maximize.')
        return 0
    print(f'Maximizing over {len(cand_pool)} cascade-demoted {args.type} candidates '
          f'(chunk_size={args.chunk_size})...', flush=True)

    with tempfile.TemporaryDirectory(prefix='cfgmax_') as tmp_str:
        tmp_dir = pathlib.Path(tmp_str)
        baseline_dir = tmp_dir / 'baseline'
        baseline_dir.mkdir()
        mirror_recomp = mirror_repo_layout(tmp_dir)
        # Snapshot mirror as the pristine starting point.
        originals = {}
        for bank in BANKS:
            originals[bank] = (mirror_recomp / f'bank{bank:02x}.cfg').read_text(
                encoding='utf-8')
        # Build baselines (= current cfg state, which already has previous
        # strips applied).
        for bank in BANKS:
            if not regen_bank(bank,
                              mirror_recomp / f'bank{bank:02x}.cfg',
                              baseline_dir / f'smw_{bank:02x}_baseline.c'):
                print(f'  baseline regen FAIL bank {bank:02x}', file=sys.stderr)
                return 1

        safe = chunked_greedy(cand_pool, mirror_recomp, baseline_dir, tmp_dir,
                              originals, chunk_size=args.chunk_size)

    print(f'\nMaximal safe subset: {len(safe)}/{len(cand_pool)} cascade-demoted '
          f'candidates are actually strippable.')

    # Patch audit JSON: set cascade_offender=False on safe candidates.
    safe_keys = {(s['bank'], s['line_no'], s['token']) for s in safe}
    flipped = 0
    for r in data['results']:
        key = (r['bank'], r['line_no'], r['token'])
        if key in safe_keys and r.get('cascade_offender'):
            r['cascade_offender'] = False
            flipped += 1
    data['summary']['redundant_safe'] = sum(
        1 for r in data['results']
        if r.get('diff_line_count') == 0 and not r.get('cascade_offender'))
    data['summary']['redundant_cascading'] = sum(
        1 for r in data['results']
        if r.get('diff_line_count') == 0 and r.get('cascade_offender'))
    cands_path.write_text(json.dumps(data, indent=2))
    print(f'Patched {cands_path.name}: flipped {flipped} cascade_offender -> False.')
    print(f'Total strippable now: {data["summary"]["redundant_safe"]}')
    print('\nRun:')
    print(f'  python snesrecomp/tools/cfg_override_strip.py --type {args.type} --apply')
    return 0


if __name__ == '__main__':
    sys.exit(main())
