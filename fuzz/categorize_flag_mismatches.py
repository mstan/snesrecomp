"""Categorize flag-capture mismatches by (mnemonic, which-flag-diff).

Queries results.db and groups failures so we can see which flag on
which mnemonic is most broken, rather than staring at 969 rows.
"""
import json
import pathlib
import sqlite3
from collections import Counter

FUZZ_DIR = pathlib.Path(__file__).resolve().parent
DB = FUZZ_DIR / 'results.db'

FLAG_SLOTS = {0x1F06: 'C', 0x1F07: 'Z', 0x1F08: 'N', 0x1F09: 'V'}


def which_flags_diff(recomp_delta: dict, oracle_delta: dict) -> set:
    """Return the set of flag letters where recomp and oracle disagree."""
    flags = set()
    for addr, letter in FLAG_SLOTS.items():
        key = f'0x{addr:x}'
        r = recomp_delta.get(key)
        o = oracle_delta.get(key)
        # Baseline is 0xFF. Delta presence = value differs from 0xFF.
        # If one side has a delta and the other doesn't, the flag differs.
        # If both have delta with the same value, they match.
        # If both absent, both 0xFF, they match.
        if r != o:
            flags.add(letter)
    return flags


def wram_reg_diff(recomp_delta: dict, oracle_delta: dict) -> bool:
    """True if the test-insn's observable register/memory output
    (A/X/Y snapshots + test memory slots) differs — i.e. a non-flag bug."""
    for key in list(recomp_delta.keys()) + list(oracle_delta.keys()):
        a = int(key, 16)
        if a in (0x1F06, 0x1F07, 0x1F08, 0x1F09):
            continue  # flag slot, handled separately
        if recomp_delta.get(key) != oracle_delta.get(key):
            return True
    return False


def main():
    con = sqlite3.connect(DB)
    cur = con.execute(
        'SELECT id, mnem, mode, m_flag, x_flag, seed_name, '
        'recomp_delta, oracle_delta '
        'FROM runs WHERE matched = 0 AND error IS NULL')

    by_mnem_flag = Counter()          # (mnem, flag) → count
    by_mnem_nonflag = Counter()       # mnem → count of non-flag divergences
    total = 0
    examples_per_bucket = {}
    for sid, mnem, mode, mf, xf, seed, rd, od in cur:
        total += 1
        r = json.loads(rd); o = json.loads(od)
        flags = which_flags_diff(r, o)
        nonflag = wram_reg_diff(r, o)
        for f in flags:
            by_mnem_flag[(mnem, f)] += 1
            k = (mnem, f)
            if k not in examples_per_bucket:
                examples_per_bucket[k] = (sid, rd, od)
        if nonflag:
            by_mnem_nonflag[mnem] += 1

    print(f'Total mismatched snippets: {total}\n')

    print('=== Flag-only divergences (mnem, flag) ===')
    for (mnem, flag), count in sorted(by_mnem_flag.items(), key=lambda kv: -kv[1]):
        print(f'  {mnem:4s} {flag}: {count:4d} snippets')
    print()

    print('=== Non-flag (register/memory) divergences by mnem ===')
    for mnem, count in sorted(by_mnem_nonflag.items(), key=lambda kv: -kv[1]):
        print(f'  {mnem:4s}: {count:4d} snippets')
    print()

    print('=== Examples ===')
    for k, v in sorted(examples_per_bucket.items())[:20]:
        mnem, flag = k
        sid, rd, od = v
        print(f'  [{mnem}/{flag}] {sid}')
        print(f'    recomp: {rd}')
        print(f'    oracle: {od}')


if __name__ == '__main__':
    main()
