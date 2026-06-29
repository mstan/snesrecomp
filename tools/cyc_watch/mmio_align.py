#!/usr/bin/env python3
"""Axis-4 MMIO write diff: recomp vs bsnes over $4200-$421F (or any range).

Both sides log (frame, addr, val) MMIO register writes while running the same
game. Boot/one-shot frames differ and the absolute frame numbers are offset,
so we compare the STEADY-STATE per-frame write PATTERN: group writes by frame
into an ordered (addr,val) tuple, count distinct patterns, and check that the
recomp's dominant attract-frame pattern also occurs on the bsnes side (and vice
versa). A divergence = the recomp drives the CPU control registers differently.

Usage: python mmio_align.py <recomp.json> <bsnes.txt>
"""
import sys, json
from collections import Counter


def load_recomp(path):
    o = json.load(open(path))
    out = []
    for e in o.get("log", []):
        out.append((int(e["f"]), int(e["adr"], 16), int(e["val"], 16)))
    return out


def load_bsnes(path):
    out = []
    for line in open(path):
        p = line.split()
        if len(p) != 3:
            continue
        out.append((int(p[0]), int(p[1], 16), int(p[2], 16)))
    return out


def per_frame_patterns(events):
    """frame -> ordered tuple of (addr,val); return Counter of those tuples."""
    byframe = {}
    for f, a, v in events:
        byframe.setdefault(f, []).append((a, v))
    return Counter(tuple(seq) for seq in byframe.values()), byframe


def fmt(pat):
    return " ".join(f"{a:04X}={v:02X}" for a, v in pat) if pat else "(empty)"


def main(argv):
    if len(argv) != 2:
        print("usage: mmio_align.py <recomp.json> <bsnes.txt>"); return 2
    rc = load_recomp(argv[0])
    bs = load_bsnes(argv[1])
    rc_pat, _ = per_frame_patterns(rc)
    bs_pat, _ = per_frame_patterns(bs)
    print(f"# recomp: {len(rc)} writes, {len(rc_pat)} distinct per-frame patterns")
    print(f"# bsnes:  {len(bs)} writes, {len(bs_pat)} distinct per-frame patterns")

    print("\n# --- recomp top per-frame patterns ---")
    for pat, n in rc_pat.most_common(6):
        inbs = "  IN-BSNES" if pat in bs_pat else "  *** NOT IN BSNES ***"
        print(f"  x{n:<4} {fmt(pat)}{inbs}")
    print("\n# --- bsnes top per-frame patterns ---")
    for pat, n in bs_pat.most_common(6):
        inrc = "  IN-RECOMP" if pat in rc_pat else "  *** NOT IN RECOMP ***"
        print(f"  x{n:<4} {fmt(pat)}{inrc}")

    # Verdict: the recomp's dominant attract pattern must occur in bsnes.
    rc_dom = rc_pat.most_common(1)[0][0] if rc_pat else None
    bs_dom = bs_pat.most_common(1)[0][0] if bs_pat else None
    print("\n# --- verdict ---")
    print(f"recomp dominant : {fmt(rc_dom)}")
    print(f"bsnes  dominant : {fmt(bs_dom)}")
    ok = (rc_dom is not None) and (rc_dom in bs_pat)
    print("MATCH: recomp dominant frame-pattern occurs in bsnes"
          if ok else "MISMATCH: recomp dominant frame-pattern NOT in bsnes")
    # also flag any recomp pattern (>=3 occ) absent from bsnes
    missing = [(p, n) for p, n in rc_pat.items() if n >= 3 and p not in bs_pat]
    if missing:
        print(f"recomp patterns (>=3 occ) absent from bsnes: {len(missing)}")
        for p, n in sorted(missing, key=lambda t: -t[1])[:6]:
            print(f"  x{n:<4} {fmt(p)}")
    else:
        print("every recurring (>=3 occ) recomp frame-pattern also occurs in bsnes")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
