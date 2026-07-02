#!/usr/bin/env python3
"""APU/SPC-RAM structural diff (co-sim audio hunt). Unlike align_diff.py's
per-frame written-mask agreement (tuned for WRAM), this reconstructs the FULL
64K SPC RAM image on both sides at an aligned frame and classifies divergence by
region, so we can tell a benign volatile-engine-state mismatch (SPC zero page /
echo buffer, which won't byte-match unless the two SPC cores are cycle-locked)
from a real STATIC divergence (uploaded driver code, sample directory, BRR
samples, sequence data — these MUST match if the same data was uploaded).

Usage: apu_region_diff.py <oracle.jsonl> <recomp.jsonl> --offset O [--frame F]
"""
import json, sys

SIZE = 0x10000

def load(path):
    frames, maxf = {}, 0
    for line in open(path):
        line = line.strip()
        if not line: continue
        try:
            d = json.loads(line); f = int(d["f"]); a = int(d["adr"], 16); v = int(d["val"], 16)
        except Exception:
            continue
        frames.setdefault(f, []).append((a, v)); maxf = max(maxf, f)
    return frames, maxf

def image_at(frames, upto):
    """Full SPC RAM image reconstructed by replaying all writes through frame `upto`."""
    img = bytearray(SIZE)
    for f in range(1, upto + 1):
        for a, v in frames.get(f, []):
            img[a] = v
    return img

def main():
    argv = sys.argv[1:]
    O = 0; frame = None
    if "--offset" in argv:
        i = argv.index("--offset"); O = int(argv[i+1], 0); del argv[i:i+2]
    if "--frame" in argv:
        i = argv.index("--frame"); frame = int(argv[i+1], 0); del argv[i:i+2]
    oracle, omax = load(argv[0]); recomp, rmax = load(argv[1])
    if frame is None: frame = rmax - 5     # near-end, well past boot
    j = frame + O
    print(f"oracle frames={omax} recomp frames={rmax}; comparing recomp f{frame} vs oracle f{j} (O={O})")
    R = image_at(recomp, frame); Ora = image_at(oracle, j)

    # region histogram over 4KB pages
    print("\nregion         diff-bytes / total   (region = 0x1000 pages)")
    total_diff = 0
    for base in range(0, SIZE, 0x1000):
        d = sum(1 for a in range(base, base+0x1000) if R[a] != Ora[a])
        total_diff += d
        bar = "#" * (d * 40 // 0x1000)
        print(f"  ${base:04x}-${base+0xfff:04x}: {d:4d}/4096  {bar}")
    print(f"\nTOTAL differing bytes: {total_diff}/{SIZE} ({100*total_diff/SIZE:.2f}%)")

    # zero-page ($00-$ff) is volatile engine state; classify
    zp = sum(1 for a in range(0x100) if R[a] != Ora[a])
    stack = sum(1 for a in range(0x100, 0x200) if R[a] != Ora[a])
    rest = total_diff - zp - stack
    print(f"  zero-page $0000-$00ff (volatile driver vars): {zp}/256")
    print(f"  stack     $0100-$01ff:                        {stack}/256")
    print(f"  rest      $0200-$ffff (code/samples/seq/echo): {rest}")

    # show the first divergences in the 'rest' region (the interesting ones)
    print("\nfirst 30 divergences at/above $0200:")
    shown = 0
    for a in range(0x200, SIZE):
        if R[a] != Ora[a]:
            print(f"   ${a:04x}: oracle=0x{Ora[a]:02x} recomp=0x{R[a]:02x}")
            shown += 1
            if shown >= 30: break

if __name__ == "__main__":
    main()
