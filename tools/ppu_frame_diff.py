#!/usr/bin/env python3
"""ppu_frame_diff.py — two-process per-frame framebuffer diff: recomp PPU output
vs the bsnes oracle, for SNES Axis-5 (PPU/video) verification.

Capture sides (both emit raw 256x224 BGRX = XRGB8888 byte order):
  recomp : debug-server `dump_frame_raw <N> <abs_path>` (runner/src/debug_server.c),
           a NON-PAUSING present-time capture. Run the game in AUTHENTIC mode
           (config.ini Widescreen=0, NoSpriteLimits=0) or the 256-wide crop grabs
           the 16:9 left-extension and won't match.
  oracle : tools/snesref with env SNESREF_FRAME_DUMP_DIR/_FROM/_TO/_STEP
           (converts bsnes 0RGB1555 -> BGRX).

The two boot at different speeds (the recomp HLEs boot), so there is a constant
frame OFFSET between them; this tool searches for it. Frames are discrete and
deterministic, so once aligned a faithful PPU is bit-identical.

Usage:
  python ppu_frame_diff.py <recomp_dir> <bsnes_dir>
  python ppu_frame_diff.py <recomp_dir> <bsnes_dir> --offset N   # force an offset
Only dependency: numpy.
"""
import numpy as np, glob, os, re, sys, argparse

W, H = 256, 224


def load(path):
    a = np.fromfile(path, dtype=np.uint8)
    if a.size != W * H * 4:
        return None
    return a.reshape(H, W, 4)[:, :, :3].astype(np.int16)  # BGR, drop X


def frames(d):
    out = {}
    for p in sorted(glob.glob(os.path.join(d, "frame_*.raw"))):
        m = re.search(r"frame_(\d+)\.raw", p)
        if m:
            img = load(p)
            if img is not None:
                out[int(m.group(1))] = img
    return out


def compare(a, b):
    d = np.abs(a - b)
    exact = float((d.max(axis=2) == 0).mean()) * 100      # exact 8-bit pixel match
    within8 = float((d.max(axis=2) <= 8).mean()) * 100    # tolerant (5->8 expand)
    mad = float(d.mean())
    mse = float((d.astype(np.float64) ** 2).mean())
    psnr = 99.0 if mse < 1e-9 else 10 * np.log10(255 * 255 / mse)
    mism = int((d.max(axis=2) > 0).sum())
    return exact, within8, mad, psnr, mism


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("recomp_dir")
    ap.add_argument("bsnes_dir")
    ap.add_argument("--offset", type=int, default=None,
                    help="force bsnes = recomp + offset (else best-match search)")
    args = ap.parse_args()
    rec, bsn = frames(args.recomp_dir), frames(args.bsnes_dir)
    if not rec or not bsn:
        print("no frames loaded"); return 2
    print(f"recomp: {sorted(rec)}")
    print(f"bsnes : {min(bsn)}..{max(bsn)} ({len(bsn)} frames)")
    print("\n recF -> bsnF  off | exact%  w8%   MAD   PSNR  mismatch_px")
    offs = []
    for rf in sorted(rec):
        if args.offset is not None:
            bf = rf + args.offset
            if bf not in bsn:
                print(f" {rf:5d} -> (bsnes {bf} not captured)"); continue
            cand = [(bf, *compare(rec[rf], bsn[bf]))]
        else:
            cand = [(bf, *compare(rec[rf], img)) for bf, img in bsn.items()]
        bf, ex, w8, mad, psnr, mism = max(cand, key=lambda c: (c[1], -c[5]))
        offs.append(bf - rf)
        print(f" {rf:5d} -> {bf:5d} {bf-rf:+5d} | {ex:6.2f} {w8:6.2f} {mad:5.2f} {psnr:6.2f} {mism:7d}")
    if args.offset is None and offs:
        from collections import Counter
        print("\nimplied offsets:", Counter(offs).most_common())
    return 0


if __name__ == "__main__":
    sys.exit(main())
