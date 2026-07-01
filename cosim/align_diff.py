#!/usr/bin/env python3
"""Boot-offset-aligned first-divergence finder for the frame-boundary co-sim
(recomp vs bsnes). Both inputs are {"f","adr","old","val"} jsonl WRAM traces
over $0000-$1FFF.

The recomp HLE-boots while bsnes runs the real boot, so their frame counters are
offset. This tool:
  1. reconstructs per-frame low-WRAM state on both sides,
  2. finds the boot offset O that maximizes game-written-byte agreement
     (recomp[i] vs oracle[i+O]),  -- alignment, NOT masking,
  3. reports the first real divergence at O, and CLASSIFIES each divergent
     address as stack ($0100-$01FF) vs game-state, so HLE-artifacts (stack /
     NMI-frame) are visible but not silently suppressed.

Usage: align_diff.py <oracle.jsonl> <recomp.jsonl> [--exclude-stack]
"""
import json, sys

WRAM = 0x2000
STACK_LO, STACK_HI = 0x0100, 0x01FF

def load(path):
    frames, maxf = {}, 0
    for line in open(path):
        line = line.strip()
        if not line: continue
        try:
            d = json.loads(line)
            f = int(d["f"]); a = int(d["adr"], 16); v = int(d["val"], 16)
        except Exception:
            continue
        if a < WRAM:
            frames.setdefault(f, []).append((a, v))
            maxf = max(maxf, f)
    return frames, maxf

def snapshots(frames, maxf):
    """Return per-frame (state bytearray, written bytearray)."""
    state = bytearray(WRAM); written = bytearray(WRAM)
    fill = None
    snaps = [None] * (maxf + 1)
    for f in range(1, maxf + 1):
        for a, v in frames.get(f, []):
            if f == 1:
                if fill is None: fill = v
            else:
                written[a] = 1
            state[a] = v
        snaps[f] = (bytes(state), bytes(written))
    return snaps

def agree_frac(sa, sb, exclude_stack):
    """Fraction of jointly-game-written bytes that AGREE between two snapshots."""
    stA, wrA = sa; stB, wrB = sb
    same = tot = 0
    for a in range(WRAM):
        if not (wrA[a] and wrB[a]): continue
        if exclude_stack and STACK_LO <= a <= STACK_HI: continue
        tot += 1
        if stA[a] == stB[a]: same += 1
    return (same / tot if tot else 0.0), tot

def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    exclude_stack = "--exclude-stack" in sys.argv
    oracle_f, omax = load(args[0])
    recomp_f, rmax = load(args[1])
    print(f"oracle frames={omax} recomp frames={rmax}  exclude_stack={exclude_stack}")
    osnap = snapshots(oracle_f, omax)
    rsnap = snapshots(recomp_f, rmax)

    # --- find boot offset O: recomp[i] <-> oracle[i+O], sampled mid-run ---
    lo, hi = -30, max(60, omax - 40)
    scores = []
    probe = [f for f in range(20, rmax, 4)]
    for O in range(lo, hi + 1):
        acc = n = 0.0
        for i in probe:
            j = i + O
            if 1 <= i <= rmax and 1 <= j <= omax and rsnap[i] and osnap[j]:
                fr, tot = agree_frac(rsnap[i], osnap[j], exclude_stack)
                if tot > 200: acc += fr; n += 1
        if n: scores.append((acc / n, O))
    scores.sort(reverse=True)
    print("top offsets (agreement%, O):", [(round(s*100,1), o) for s, o in scores[:6]])
    score, O = scores[0]
    print(f"BEST boot offset O={O} (recomp[i] vs oracle[i+O]); mid-run agreement {score*100:.1f}%")

    # --- skip the boot transition: start where the offset alignment stabilizes ---
    start = 2
    for i in range(2, rmax + 1):
        j = i + O
        if 1 <= j <= omax and rsnap[i] and osnap[j]:
            fr, tot = agree_frac(rsnap[i], osnap[j], exclude_stack)
            if tot > 200 and fr > 0.98: start = i; break
    print(f"alignment stabilizes at recomp frame {start}; scanning from there")

    # --- first real divergence at offset O (post-boot) ---
    for i in range(start, rmax + 1):
        j = i + O
        if not (1 <= j <= omax and rsnap[i] and osnap[j]): continue
        stR, wrR = rsnap[i]; stO, wrO = osnap[j]
        diffs = [a for a in range(WRAM)
                 if wrR[a] and wrO[a] and stR[a] != stO[a]
                 and not (exclude_stack and STACK_LO <= a <= STACK_HI)]
        if diffs:
            stk = [a for a in diffs if STACK_LO <= a <= STACK_HI]
            game = [a for a in diffs if not (STACK_LO <= a <= STACK_HI)]
            print(f"\nFIRST DIVERGENCE @ recomp frame {i} (oracle {j}): "
                  f"{len(diffs)} addrs  [{len(game)} game-state, {len(stk)} stack]")
            for a in diffs[:24]:
                tag = " (stack)" if STACK_LO <= a <= STACK_HI else ""
                print(f"   ${a:05x}: oracle=0x{stO[a]:02x} recomp=0x{stR[a]:02x}{tag}")
            if game:
                print(f"  >>> first GAME-STATE divergence: ${game[0]:05x}")
            else:
                print("  (all divergences are in the stack page — HLE/NMI-frame artifact)")
            break
    else:
        print(f"\nNO divergence at offset O={O} across the overlap — recomp matches bsnes.")

    # --- PERSISTENCE: a real divergence stays wrong; boundary noise flickers ---
    from collections import Counter
    diffcnt, nframes = Counter(), 0
    for i in range(start, rmax + 1):
        j = i + O
        if not (1 <= j <= omax and rsnap[i] and osnap[j]): continue
        nframes += 1
        stR, wrR = rsnap[i]; stO, wrO = osnap[j]
        for a in range(WRAM):
            if wrR[a] and wrO[a] and stR[a] != stO[a]:
                diffcnt[a] += 1
    print(f"\nPERSISTENCE over {nframes} aligned frames (addr: %frames-differing):")
    persistent_game = []
    for a, c in sorted(diffcnt.items(), key=lambda kv: -kv[1])[:20]:
        pct = 100 * c / max(1, nframes)
        tag = " STACK" if STACK_LO <= a <= STACK_HI else ""
        if pct > 60 and not (STACK_LO <= a <= STACK_HI): persistent_game.append(a)
        print(f"   ${a:05x}: {pct:5.1f}%{tag}")
    print(f"\n>>> PERSISTENT (>60%) non-stack divergences: "
          f"{['$%05x'%a for a in persistent_game] or 'NONE — residual is boundary/HLE noise'}")

if __name__ == "__main__":
    main()
