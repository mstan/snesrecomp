#!/usr/bin/env python3
"""Pick clean Axis-2 cycle-diff regions from a recomp per-block cycle ring.

Input: a `cyc_ring` dump — lines "seq 0xPC cycles" (cycles = cpu->cycles
BEFORE that block's charge). The d between consecutive entries is exactly
one block's emitted charge; the two PCs bracket exactly that block's
instructions in bsnes too.

We surface candidate regions for the bsnes probe:
  - SINGLE-BLOCK: transition pc_i -> pc_{i+1}; recomp d = charge(block_i).
    We keep only transitions whose d is IDENTICAL across every occurrence
    (data-independent control flow) so bsnes's reset-first-hit latch over
    the same [pc_i, pc_{i+1}) PC pair measures the same work.

Usage: python ring_pick.py <ring.txt> [topN]
"""
import sys
from collections import defaultdict


def main(argv):
    if not argv:
        print("usage: ring_pick.py <ring.txt> [topN]"); return 2
    path = argv[0]
    topN = int(argv[1]) if len(argv) > 1 else 20
    seqs = []
    for line in open(path):
        p = line.split()
        if len(p) != 3:
            continue
        seq = int(p[0]); pc = int(p[1], 16); cyc = int(p[2])
        seqs.append((seq, pc, cyc))
    seqs.sort()
    # transition (pc_i -> pc_next) -> list of deltas
    trans = defaultdict(list)
    for i in range(len(seqs) - 1):
        _, pc, cyc = seqs[i]
        nseq, npc, ncyc = seqs[i + 1]
        if nseq != seqs[i][0] + 1:
            continue  # ring wrap / gap
        d = ncyc - cyc
        if d <= 0:
            continue
        trans[(pc, npc)].append(d)

    cands = []
    for (pc, npc), ds in trans.items():
        constant = len(set(ds)) == 1
        if not constant:
            continue
        cands.append((ds[0], len(ds), pc, npc))
    # sort: prefer many occurrences, then larger delta (meatier region)
    cands.sort(key=lambda t: (t[1], t[0]), reverse=True)

    # Loop-exit edges: start PC that ALSO self-loops (start->start) but here
    # exits to a different end. These are the data-dependent regions the TIGHT
    # latch isolates (last start before end = the exit iteration).
    self_loopers = {pc for (pc, npc) in trans if pc == npc}
    exits = [(d, n, pc, npc) for (d, n, pc, npc) in cands
             if pc in self_loopers and pc != npc]
    exits.sort(key=lambda t: (t[1], t[0]), reverse=True)

    print(f"# {len(seqs)} ring entries, {len(trans)} distinct transitions, "
          f"{len(cands)} constant-d single-block candidates, "
          f"{len(self_loopers)} self-looping PCs")
    print(f"# --- clean (non-self-loop start) regions ---")
    print(f"# {'delta':>6} {'count':>6}  start_pc   end_pc")
    for d, n, pc, npc in cands[:topN]:
        if pc in self_loopers or pc == npc:
            continue
        print(f"  {d:>6} {n:>6}  0x{pc:06X} 0x{npc:06X}")
    print(f"# --- loop-exit edges (start self-loops; tight latch isolates) ---")
    for d, n, pc, npc in exits[:topN]:
        print(f"  {d:>6} {n:>6}  0x{pc:06X} 0x{npc:06X}")
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
