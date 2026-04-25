"""Phase B differential fuzz differ.

Read recomp_final.jsonl and oracle_final.jsonl, filter out oracle
noise (snippet bytes written to WRAM + stack-seed bytes at $1FE/$1FF),
and report which snippets diverge. Writes a SQLite DB summarizing
current coverage and any regressions.

The comparison regions we care about:
  $1F00-$1F05  epilogue-snapshot A/X/Y
  $10, $11     DP test bytes (TSB/TRB/ASL etc writes here)
  $100, $101   ABS test bytes
Everything else is setup artifact.
"""
from __future__ import annotations
import json
import pathlib
import sqlite3
import sys

FUZZ_DIR = pathlib.Path(__file__).resolve().parent
RESULTS = FUZZ_DIR / 'results'
RECOMP = RESULTS / 'recomp_final.jsonl'
ORACLE = RESULTS / 'oracle_final.jsonl'
DB = FUZZ_DIR / 'results.db'
SNIPPETS = FUZZ_DIR / 'snippets' / 'snippets.json'


# Oracle-side setup noise: snippet bytes written to WRAM at the
# snippet PC range, plus the stack region above the snippet's working
# space. Recomp doesn't write these addresses because it tracks the
# stack logically rather than as bytes in memory.
#
# Snippet PC is bank $00 $1800; longest snippet is ~50 bytes, so
# $1800-$18FF covers the snippet region safely.
# Stack region: the seeded sentinel lives at S.W=0x1FF and 0x1FE.
# Compound snippets that PHA/PHX/PHY/PLA/PLX/PLY also touch lower
# stack addresses ($1FC/$1FD/etc.) — filter the whole upper-page area
# below the sentinel down to $1F00 (the flag-capture region).
ORACLE_NOISE = set()
ORACLE_NOISE.update(range(0x1800, 0x1900))
# Stack page area touched by the snippet: seeded sentinel uses $1FE/$1FF,
# compound PHA/PLA snippets dip a few bytes lower. Safe filter window:
# $1F8-$1FF (won't intersect with the $0100/$0101 ABS test slots, which
# are several pages away from the stack-page tail).
ORACLE_NOISE.update(range(0x1F8, 0x200))


def filter_delta(delta: dict, side: str) -> dict:
    """Drop oracle-only setup-noise addresses; keep everything else so
    the filter is symmetric and catches REAL divergences at any WRAM
    address the test instruction may touch."""
    out = {}
    for k, v in delta.items():
        a = int(k, 16)
        if side == 'oracle' and a in ORACLE_NOISE:
            continue
        out[k] = v
    return out


def load_jsonl(path: pathlib.Path) -> dict:
    out = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line: continue
            r = json.loads(line)
            out[r['id']] = r
    return out


def main():
    if not RECOMP.exists():
        print('recomp results missing; run run_recomp.py first', file=sys.stderr)
        raise SystemExit(2)
    if not ORACLE.exists():
        print('oracle results missing; run run_oracle.py first', file=sys.stderr)
        raise SystemExit(2)

    rec = load_jsonl(RECOMP)
    orc = load_jsonl(ORACLE)
    snippets = {s['id']: s for s in json.load(open(SNIPPETS))}

    con = sqlite3.connect(DB)
    con.executescript('''
      DROP TABLE IF EXISTS runs;
      CREATE TABLE runs (
        id TEXT PRIMARY KEY,
        mnem TEXT, mode TEXT, m_flag INT, x_flag INT, seed_name TEXT,
        matched INT,
        recomp_delta TEXT,
        oracle_delta TEXT,
        error TEXT
      );
    ''')

    ok = 0
    mismatch = 0
    error = 0
    rows = []
    for sid, snip in snippets.items():
        r = rec.get(sid)
        o = orc.get(sid)
        err = None
        matched = 0
        r_d = {}
        o_d = {}

        if r is None or 'wram_delta' not in r:
            err = 'missing recomp'
            error += 1
        elif o is None:
            err = 'missing oracle'
            error += 1
        elif 'error' in o:
            err = f'oracle error: {o["error"]}'
            error += 1
        else:
            r_d = filter_delta(r['wram_delta'], 'recomp')
            o_d = filter_delta(o['wram_delta'], 'oracle')
            if r_d == o_d:
                matched = 1
                ok += 1
            else:
                mismatch += 1

        rows.append((sid, snip['mnem'], snip['mode'],
                     snip['m_flag'], snip['x_flag'], snip['seed_name'],
                     matched, json.dumps(r_d), json.dumps(o_d), err))

    con.executemany(
        'INSERT INTO runs VALUES (?,?,?,?,?,?,?,?,?,?)', rows)
    con.commit()

    print(f'total snippets:    {len(snippets)}')
    print(f'  matched:         {ok}')
    print(f'  mismatched:      {mismatch}')
    print(f'  error:           {error}')

    if mismatch:
        print(f'\nFirst 20 mismatches:')
        cur = con.execute(
            'SELECT id, recomp_delta, oracle_delta FROM runs '
            'WHERE matched=0 AND error IS NULL LIMIT 20')
        for sid, rd, od in cur:
            print(f'  {sid}')
            print(f'    recomp: {rd}')
            print(f'    oracle: {od}')


if __name__ == '__main__':
    main()
