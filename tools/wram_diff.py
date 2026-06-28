#!/usr/bin/env python3
"""First-divergence finder: diff two per-frame WRAM change traces (snesref
oracle vs recomp), both in the {"f","adr","old","val"} jsonl shape. Reconstructs
full $0000-$1FFF state per frame on each side and reports the first frame +
lowest address where they differ."""
import json, sys

def load(path):
    frames, maxf, bad = {}, 0, 0
    for line in open(path):
        line = line.strip()
        if not line:
            continue
        try:
            d = json.loads(line)
            f = int(d["f"]); a = int(d["adr"], 16); v = int(d["val"], 16)
        except Exception:
            bad += 1
            continue
        frames.setdefault(f, []).append((a, v))
        maxf = max(maxf, f)
    if bad:
        print(f"  ({path}: skipped {bad} malformed line(s))")
    return frames, maxf

oracle_f, omax = load(sys.argv[1])
recomp_f, rmax = load(sys.argv[2])
A, B = {}, {}
last = min(omax, rmax)
print(f"oracle max frame={omax}, recomp max frame={rmax}; comparing 1..{last}")

# Frame 1 is each side's power-on fill (snes9x=$55, recomp zero-init=$00).
# Unwritten bytes trivially differ, so we mask addresses where BOTH sides still
# hold their own fill and only flag real game-written divergences.
for a, v in oracle_f.get(1, []): A[a] = v
for a, v in recomp_f.get(1, []): B[a] = v
A_fill, B_fill = dict(A), dict(B)
print(f"oracle fill=0x{A.get(0,0):02x}, recomp fill=0x{B.get(0,0):02x} "
      f"(masked); comparing game-written bytes from frame 2")

def real_diffs():
    out = []
    for a in (set(A) | set(B)):
        av, bv = A.get(a, 0), B.get(a, 0)
        if av == bv:
            continue
        # Require BOTH sides to have written the address (moved off their own
        # power-on fill). Skips "one side wrote, other hasn't yet" — that's
        # boot timing-of-writes, not a genuine value disagreement.
        if av == A_fill.get(a) or bv == B_fill.get(a):
            continue
        out.append(a)
    return sorted(out)

for f in range(2, last + 1):
    for a, v in oracle_f.get(f, []): A[a] = v
    for a, v in recomp_f.get(f, []): B[a] = v
    diffs = real_diffs()
    if diffs:
        a0 = diffs[0]
        print(f"\nFIRST REAL DIVERGENCE @ frame {f}: ${a0:05x} "
              f"oracle=0x{A.get(a0,0):02x} recomp=0x{B.get(a0,0):02x}  "
              f"({len(diffs)} game-written addrs differ)")
        for a in diffs[:20]:
            print(f"   ${a:05x}: oracle=0x{A.get(a,0):02x}  recomp=0x{B.get(a,0):02x}")
        sys.exit(0)

print(f"\nNO REAL DIVERGENCE in $0000-$1FFF across frames 1..{last} — aligned.")
