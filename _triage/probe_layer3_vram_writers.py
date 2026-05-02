"""Layer-3/HUD VRAM corruption probe.

Background: user observed HUD/border tile-graphics corruption (post-LSR-
fix). Earlier full-VRAM scan showed 5847 byte diffs vs the snes9x oracle,
concentrated in word-VRAM regions consistent with Layer-3/HUD tile data
($0900-$1DFF, $5200-$5FFF, $7500-$84FF in BYTE coords; word coords
$0480-$0EFF, $2900-$2FFF, $3A80-$4280 etc).

This probe focuses on the $2900-$2FFF word region (the densest divergence
in the previous scan). Strategy — per global rule "never arm-then-attach":

  1. The runner already arms `s_vram_trace` for the full VRAM word range
     at boot (debug_server_init, SNESRECOMP_REVERSE_DEBUG). The ring is
     65536 entries deep and capturing every word VRAM write continuously
     from process start.
  2. We launch in --paused, run for N frames into the attract demo,
     pause, then query `get_vram_trace nostack` and post-process to
     count writers per address word.
  3. Output: top-10 functions writing into $2900-$2FFF, with hit counts.
     This identifies the upload routine to target next.

We do NOT call `trace_vram` to arm — it's already armed. We do NOT
reset the ring (would lose the boot/init writes that matter most).
"""
import socket, sys, json, time
from collections import Counter

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 4377
RUN_FRAMES = 30  # ~0.5s — captures boot HUD/font upload before ring fills.
# At ~530 VRAM writes per (recomp + oracle) frame, ring fills near frame 120;
# the JSON-budget caps the returned-window at ~7400 entries; staying below
# frame 30 ensures we see the FIRST writes (boot/init).
# Target the word-coord region the user's earlier full-VRAM diff flagged
# as Layer-3/HUD-corrupt. Trace addrs are word coords (see trace_vram doc).
WORD_LO, WORD_HI = 0x5200, 0x5FFF


_pending = b""


def recv_line(s):
    global _pending
    while b"\n" not in _pending:
        chunk = s.recv(1 << 18)
        if not chunk:
            break
        _pending += chunk
    nl = _pending.find(b"\n")
    if nl < 0:
        out, _pending = _pending, b""
    else:
        out, _pending = _pending[:nl], _pending[nl + 1:]
    return out.decode(errors="replace").strip()


def cmd(s, line):
    s.sendall((line + "\n").encode())
    return recv_line(s)


def main():
    s = socket.create_connection(("127.0.0.1", PORT), timeout=5)
    s.settimeout(15)
    # Server emits a "connected" banner on accept — drain it before
    # issuing the first command, otherwise replies end up shifted
    # by one line.
    print("banner:", recv_line(s))
    print("ping:", cmd(s, "ping"))

    # Free-run for ~5 s, then pause and query. Step has a hard 4.5 s
    # internal timeout that won't reach 240 frames if the runtime is
    # any slower than vsync; continue/pause is the deterministic path.
    print("continue:", cmd(s, "continue"))
    print(f"sleeping {RUN_FRAMES/60:.1f}s for ~{RUN_FRAMES} frames...")
    time.sleep(RUN_FRAMES / 60.0 + 0.5)
    print("pause:", cmd(s, "pause"))
    print("ping:", cmd(s, "ping"))

    # Pull VRAM trace WITH stack info — we want to attribute writers.
    print("fetching vram trace (with stack)...")
    raw = cmd(s, "get_vram_trace")
    try:
        d = json.loads(raw)
    except json.JSONDecodeError as e:
        print("BAD JSON:", e)
        print(raw[:800])
        return 1

    if "error" in d:
        print("trace error:", d["error"])
        return 1

    log = d.get("log", [])
    print(f"trace: {len(log)} entries (count={d.get('entries')})")

    in_region = [e for e in log
                 if WORD_LO <= int(e["adr"], 16) <= WORD_HI]
    print(f"writes into ${WORD_LO:04X}-${WORD_HI:04X}: {len(in_region)}")

    # Diagnostic histogram of address words actually present in the
    # returned trace, so we can see whether the buffer-cap-truncation
    # cut the target region away or whether the region simply isn't
    # being written via WriteVramWord.
    addr_hist = Counter()
    for e in log:
        a = int(e["adr"], 16)
        addr_hist[(a & 0xFF00)] += 1
    print("\nAddress-word page histogram (top 12 by hit count):")
    for page, n in addr_hist.most_common(12):
        print(f"  ${page:04X}-${page|0xFF:04X}  {n}")

    if not in_region:
        print("(no writes in target region in returned window — buffer may"
              " have rotated past them, or path is DMA-only)")
        return 0

    by_func = Counter(e["func"] for e in in_region)
    print("\nTop writers in target region:")
    for func, n in by_func.most_common(10):
        print(f"  {n:6d}  {func}")

    # Stack-frame attribution: collapse each entry's full stack into a
    # tuple, count the unique stacks. Identifies the dominant call path.
    by_stack = Counter()
    for e in in_region:
        stk = tuple(e.get("stack", []) or [])
        by_stack[stk] += 1
    print("\nTop call stacks in target region:")
    for stk, n in by_stack.most_common(5):
        print(f"  {n:6d}  {' -> '.join(stk[-6:]) if stk else '(empty)'}")

    # Sample first 5 writes for context (which addrs, vals, frames).
    print("\nFirst 5 writes:")
    for e in in_region[:5]:
        print(f"  f={e['f']:5d}  word={e['adr']}  val={e['val']}  func={e['func']}")
    print("Last 5 writes:")
    for e in in_region[-5:]:
        print(f"  f={e['f']:5d}  word={e['adr']}  val={e['val']}  func={e['func']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
