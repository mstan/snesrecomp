#!/usr/bin/env python3
"""Alignment scan: is the recomp<->oracle mismatch a fixable frame OFFSET
(same work, different labels) or unalignable? Pin a recomp frame R; for each
oracle frame O, over the bytes the RECOMP has written (off its fill), count
how many the oracle AGREES on vs genuinely disagrees (oracle also wrote, but
differs). Best alignment = max agreements."""
import json, sys

def load(path):
    frames, maxf = {}, 0
    for line in open(path):
        line = line.strip()
        if not line: continue
        try:
            d = json.loads(line); f=int(d["f"]); a=int(d["adr"],16); v=int(d["val"],16)
        except Exception: continue
        frames.setdefault(f, []).append((a, v)); maxf = max(maxf, f)
    return frames, maxf

of, om = load(sys.argv[1]); rf, rm = load(sys.argv[2])
def state_at(frames, upto):
    s = {}
    for f in range(1, upto+1):
        for a, v in frames.get(f, []): s[a] = v
    return s
A_fill, B_fill = state_at(of, 1), state_at(rf, 1)
R = int(sys.argv[3]) if len(sys.argv) > 3 else 250
Bstate = state_at(rf, R)
Bwritten = [a for a in Bstate if Bstate[a] != B_fill.get(a)]
print(f"recomp frame {R}: {len(Bwritten)} bytes written off-fill")

Ast = {}; best = (-1, 0, -1)  # (agree, disagree, O)
for O in range(1, om+1):
    for a, v in of.get(O, []): Ast[a] = v
    if O < 2: continue
    agree = dis = 0
    for a in Bwritten:
        av = Ast.get(a, 0); bv = Bstate[a]
        if av == bv: agree += 1
        elif av != A_fill.get(a): dis += 1   # oracle also wrote it -> genuine
    if agree > best[0]: best = (agree, dis, O)
agree, dis, O = best
print(f"best alignment: oracle frame {O} (offset R-O={R-O}): "
      f"{agree}/{len(Bwritten)} agree, {dis} genuine disagreements")
