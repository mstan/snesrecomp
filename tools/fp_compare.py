#!/usr/bin/env python3
"""fp_compare.py — Axis-7 determinism check. Compares two per-frame WRAM
fingerprint dumps (debug-server `fingerprint <path> [count]`) from independent
runs of the same ROM from reset. Identical hashes on every overlapping frame =
the recompiler is bit-for-bit deterministic (which every other diff loop —
audio / PPU / cycle — silently presupposes).

Usage:  python fp_compare.py run1.txt run2.txt
Each dump line is "<frame> <hex64>". Only numpy-free stdlib.
"""
import sys


def load(p):
    d = {}
    for ln in open(p):
        a = ln.split()
        if len(a) == 2:
            d[int(a[0])] = a[1]
    return d


def main():
    if len(sys.argv) != 3:
        print("usage: fp_compare.py run1.txt run2.txt"); return 2
    r1, r2 = load(sys.argv[1]), load(sys.argv[2])
    common = sorted(set(r1) & set(r2))
    if not common:
        print("no overlapping frames (capture closer in wall time)"); return 2
    mism = [f for f in common if r1[f] != r2[f]]
    print(f"run1 {min(r1)}..{max(r1)}, run2 {min(r2)}..{max(r2)}; "
          f"{len(common)} overlapping; {len(common) - len(mism)}/{len(common)} match")
    if mism:
        print("DIVERGENCES (non-deterministic!):")
        for f in mism[:20]:
            print(f"  frame {f}: {r1[f]} vs {r2[f]}")
        return 1
    print("DETERMINISTIC: identical WRAM fingerprint on every overlapping frame.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
