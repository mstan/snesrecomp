#!/usr/bin/env python3
"""Render BGRX 256x224 .raw frames to PNG + a diff mask. For PPU eyeballing.
Usage: raw_to_png.py <recomp.raw> <bsnes.raw> <out_prefix>
"""
import sys, numpy as np
try:
    from PIL import Image
except ImportError:
    print("need Pillow"); sys.exit(2)

W, H = 256, 224
def load(p):
    a = np.fromfile(p, dtype=np.uint8)
    if a.size != W*H*4:
        print(f"bad size {a.size} for {p}"); sys.exit(2)
    bgr = a.reshape(H, W, 4)[:, :, :3]
    return bgr[:, :, ::-1]  # BGR -> RGB

r = load(sys.argv[1]); b = load(sys.argv[2]); pref = sys.argv[3]
Image.fromarray(r, 'RGB').save(pref + "_recomp.png")
Image.fromarray(b, 'RGB').save(pref + "_bsnes.png")
d = np.abs(r.astype(np.int16) - b.astype(np.int16)).max(axis=2)
mask = np.zeros((H, W, 3), np.uint8)
mask[d > 0] = (255, 0, 255)   # magenta where ANY channel differs
Image.fromarray(mask, 'RGB').save(pref + "_diffmask.png")
print(f"diff pixels: {(d>0).sum()} / {W*H}  ({100*(d>0).mean():.2f}%)")
# per-row diff histogram (where vertically the diffs cluster)
rows = (d > 0).sum(axis=1)
hot = [(int(y), int(c)) for y, c in enumerate(rows) if c > 0]
print("rows with diffs (y:count):", hot[:40])
