#!/usr/bin/env python3
"""Track specific WRAM addresses across both aligned traces around a frame window,
to characterize a divergence (RNG churn vs clean state flip vs event trigger).
Usage: track_addrs.py <oracle.jsonl> <recomp.jsonl> --offset O --frames LO HI --addrs a,b,c"""
import json, sys
argv = sys.argv[1:]
def take(f, n=1):
    i = argv.index(f); v = argv[i+1:i+1+n]; del argv[i:i+1+n]; return v
O = int(take("--offset")[0], 0)
lo, hi = [int(x) for x in take("--frames", 2)]
addrs = [int(x, 16) for x in take("--addrs")[0].split(",")]
oracle_p, recomp_p = argv[0], argv[1]

def replay(path, addrs):
    """value of each tracked addr at each frame (carry-forward)."""
    cur = {a: 0 for a in addrs}
    per = {}
    aset = set(addrs)
    for line in open(path):
        line = line.strip()
        if not line: continue
        try:
            d = json.loads(line); f = int(d["f"]); a = int(d["adr"], 16); v = int(d["val"], 16)
        except Exception:
            continue
        if a in aset: cur[a] = v
        per[f] = dict(cur)   # snapshot after this frame's writes
    return per

po = replay(oracle_p, addrs); pr = replay(recomp_p, addrs)
def snap(per, f):
    # nearest frame <= f
    ff = max((k for k in per if k <= f), default=None)
    return per.get(ff, {a:0 for a in addrs})
hdr = "  ".join(f"${a:04x}" for a in addrs)
print(f"frame  | ORACLE[f+{O}]              | RECOMP[f]")
print(f"       | {hdr}   | {hdr}")
for f in range(lo, hi+1):
    so = snap(po, f+O); sr = snap(pr, f)
    os_ = " ".join(f"{so[a]:02x}   " for a in addrs)
    rs_ = " ".join(("*" if so[a]!=sr[a] else " ")+f"{sr[a]:02x}  " for a in addrs)
    print(f"r{f:4d} | {os_} | {rs_}")
