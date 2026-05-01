"""Block-grain divergence probe for UploadLevelTilemapHDMA at frame 95.

Recomp side: query the always-on g_cpu_trace_ring (1M block events, full
CpuState per block) via trace_get_v2 — paginate backward, filter to PC
range and BLOCK event_type. Sequence within each call.

Oracle side: query emu_get_insn_trace at the same PC range — find the
matching call at frame 183. For each recomp block-PC, find the oracle
insn at the same PC and compare A/X/Y/m/xf.

This is the 'state-pinned diff at every basic block' approach from
docs/GOLDEN_TESTING.md Layer 2.
"""
import argparse, json, socket, sys

p = argparse.ArgumentParser()
p.add_argument("--port", type=int, default=4377)
p.add_argument("--pc-lo", default="0x008720", type=lambda v: int(v, 0))
p.add_argument("--pc-hi", default="0x0087ac", type=lambda v: int(v, 0))
p.add_argument("--rec-frame", type=int, default=95)
p.add_argument("--ora-frame", type=int, default=183)
args = p.parse_args()

s = socket.create_connection(("127.0.0.1", args.port), timeout=10)
s.settimeout(60)
buf = [b""]


def rl():
    while b"\n" not in buf[0]:
        c = s.recv(1 << 18)
        if not c:
            break
        buf[0] += c
    nl = buf[0].find(b"\n")
    out, buf[0] = buf[0][:nl], buf[0][nl + 1:]
    return out.decode(errors="replace").strip()


def cmd(line):
    s.sendall((line + "\n").encode())
    return rl()


print("banner:", rl())

# ------ Recomp BLOCK events ------
# trace_get_v2 walks BACKWARD from g_cpu_trace_idx. event=0 is CPU_TR_BLOCK.
# Paginate using before_idx. Collect events with PC in our range.
# 1M ring; at ~1000 blocks/frame, retains ~1000 frames; frame 95 is in range.
rec_blocks = []
before_idx = -1
total_scanned = 0
# Scan up to the full ring (16M @ default). Stop when we've seen the
# function entry PC OR the ring runs out.
SCAN_PAGE = 4096
MAX_PAGES = 16384  # 64M events upper bound — covers any heap ring size
saw_entry = False
for page in range(MAX_PAGES):
    line = f"trace_get_v2 count={SCAN_PAGE} event=0"
    if before_idx >= 0:
        line += f" before_idx={before_idx}"
    raw = cmd(line)
    try:
        d = json.loads(raw)
    except json.JSONDecodeError:
        print("rec parse err:", raw[:200])
        break
    evts = d.get("events", [])
    if not evts:
        break
    for e in evts:
        pc = int(e["pc24"], 16)
        if args.pc_lo <= pc <= args.pc_hi:
            rec_blocks.append(e)
            if pc == 0x00871E or pc == 0x008720 or pc == 0x008723:
                saw_entry = True
    total_scanned += len(evts)
    last_idx = evts[-1]["idx"]
    before_idx = int(last_idx)
    # Stop when we hit the start of the ring (idx==0)
    if before_idx == 0:
        break

# events are in REVERSE order; flip to chronological
rec_blocks.reverse()
print(f"recomp: scanned {total_scanned} BLOCK events, {len(rec_blocks)} in PC range")
if rec_blocks:
    print(f"  first: idx={rec_blocks[0]['idx']} pc={rec_blocks[0]['pc24']}")
    print(f"  last:  idx={rec_blocks[-1]['idx']} pc={rec_blocks[-1]['pc24']}")

# ------ Oracle insn trace ------
ora_log = []
from_idx = 0
ora_total = None
while True:
    raw = cmd(f"emu_get_insn_trace from={from_idx} limit=4096 "
              f"pc_lo={args.pc_lo:x} pc_hi={args.pc_hi:x}")
    d = json.loads(raw)
    ora_total = d.get("total", 0)
    chunk = d.get("log", [])
    if not chunk:
        break
    ora_log.extend(chunk)
    from_idx += 4096
    if from_idx >= ora_total:
        break
    # cap collection at 200K hits to avoid mem blow-up
    if len(ora_log) > 200000:
        break

print(f"oracle: total={ora_total}, in-range collected={len(ora_log)}")

# ------ Find the call instance: filter rec_blocks to ones at frame ~95
# and oracle to ones at frame ~183. Recomp block-trace doesn't have a
# frame field directly in the JSON above — but the events are
# chronological idx-ordered, and we can identify the call by walking
# from the FIRST block at PC=0x8720 (function entry) until we leave
# the function.
#
# We don't filter by frame — we filter by call-instance: the first
# contiguous run of in-range PCs starting at function entry is "call 1".

def split_calls(events, entry_pc, get_pc):
    """Split a chronological event list into per-call instances.
    Each call starts at entry_pc and ends when we see the next entry_pc."""
    calls = []
    cur = []
    for e in events:
        pc = get_pc(e)
        if pc == entry_pc:
            if cur:
                calls.append(cur)
            cur = [e]
        else:
            if cur:
                cur.append(e)
    if cur:
        calls.append(cur)
    return calls


rec_calls = split_calls(rec_blocks, 0x00871E, lambda e: int(e["pc24"], 16))
print(f"recomp: {len(rec_calls)} calls into UploadLevelTilemapHDMA detected")

# Find first call that ENTERS the body (i.e., goes past 0x872A — there's
# more than just entry/test/RTS).
def is_real(call, get_pc):
    pcs = {get_pc(e) for e in call}
    return 0x00872D in pcs

rec_real = [(i, c) for i, c in enumerate(rec_calls)
            if is_real(c, lambda e: int(e["pc24"], 16))]
print(f"  real (body-entering) calls: {len(rec_real)}; first at idx {rec_real[0][0] if rec_real else 'none'}")
if rec_real:
    print(f"  first real call has {len(rec_real[0][1])} blocks")

ora_calls = split_calls(
    ora_log, 0x00871E,
    lambda e: e["pc"] if isinstance(e["pc"], int) else int(e["pc"], 16))
print(f"oracle: {len(ora_calls)} calls visible")
ora_real = [(i, c) for i, c in enumerate(ora_calls)
            if is_real(c, lambda e: e["pc"] if isinstance(e["pc"], int) else int(e["pc"], 16))]
print(f"  real (body-entering) calls: {len(ora_real)}; first at idx {ora_real[0][0] if ora_real else 'none'}")
if ora_real:
    print(f"  first real call has {len(ora_real[0][1])} insns")

if not rec_real or not ora_real:
    print("NO BODY-ENTERING CALLS found.")
    sys.exit(0)

# Pick the FIRST body-entering call on each side
rec = rec_real[0][1]
ora_full_match = ora_real[0][1]

# Find oracle call whose entry state Y matches recomp's entry state Y.
# That's an apples-to-apples comparison (same stripe loaded).
def entry_y(call, get_y):
    for e in call:
        return get_y(e)
    return None

rec_entry_y = int(rec[0]["Y"], 16)
print(f"\nrec entry Y = 0x{rec_entry_y:04x}")

# Print entry states for first 30 oracle real calls
print("oracle real calls entry states (first 30):")
for i, (idx, c) in enumerate(ora_real[:30]):
    e = c[0]
    a = e["a"] if isinstance(e["a"], int) else int(e["a"], 16)
    x = e["x"] if isinstance(e["x"], int) else int(e["x"], 16)
    y = e["y"] if isinstance(e["y"], int) else int(e["y"], 16)
    f = e.get("f", "?")
    match = "  <-- Y matches recomp" if y == rec_entry_y else ""
    print(f"  ora call {idx} (frame {f}): A=0x{a:04x} X=0x{x:04x} Y=0x{y:04x}{match}")

# Find oracle call whose entry Y matches recomp's
matching_ora = None
for idx, c in ora_real:
    y = c[0]["y"] if isinstance(c[0]["y"], int) else int(c[0]["y"], 16)
    if y == rec_entry_y:
        matching_ora = c
        break

if matching_ora is None:
    print(f"\nNo oracle call has entry Y={rec_entry_y:#06x} — recomp's first body call is for "
          f"a stripe oracle never loads. Bug is UPSTREAM — caller passes different $12.")
    print("Comparing recomp's first body call against oracle's first body call as-is:")
    ora = ora_full_match
else:
    print(f"\nFound matching oracle call. Using it for comparison.")
    ora = matching_ora

print(f"COMPARE first BODY call: rec={len(rec)} blocks vs oracle={len(ora)} insns")

# For each recomp block, find the matching insn on oracle side at the same PC
# at the same sequence index (i.e., walk both forward).
print(f"\n  {'idx':>3} {'pc':>10}  | rec  A    X    Y    m x  | ora  A    X    Y    m x  | diff?")
print("  " + "-" * 95)


def fmt_h(v):
    if isinstance(v, str):
        if v.startswith("0x"):
            return v[2:]
        return v
    return f"{v:04x}"


# Walk recomp blocks; for each, find oracle insn at same PC starting from
# our cursor. This handles cases where oracle has more insns per block.
ora_cursor = 0
first_div_idx = None
for ri, rb in enumerate(rec):
    rec_pc = int(rb["pc24"], 16)
    rec_a = int(rb["A"], 16)
    rec_x = int(rb["X"], 16)
    rec_y = int(rb["Y"], 16)
    rec_m = rb["m"]
    rec_xf = rb["x"]
    # Search forward in oracle for the same PC
    found = None
    for oi in range(ora_cursor, len(ora)):
        opc = ora[oi]["pc"] if isinstance(ora[oi]["pc"], int) else int(ora[oi]["pc"], 16)
        if opc == rec_pc:
            found = oi
            break
    if found is None:
        print(f"  {ri:>3} 0x{rec_pc:06x}  | rec block, NO matching oracle insn at this PC (cursor={ora_cursor})")
        continue
    ora_cursor = found + 1
    o = ora[found]
    ora_pc = o["pc"] if isinstance(o["pc"], int) else int(o["pc"], 16)
    ora_a = o["a"] if isinstance(o["a"], int) else int(o["a"], 16)
    ora_x = o["x"] if isinstance(o["x"], int) else int(o["x"], 16)
    ora_y = o["y"] if isinstance(o["y"], int) else int(o["y"], 16)
    ora_m = o["m"]
    ora_xf = o.get("xf", o.get("x_flag", 0))
    diff = []
    if rec_m != ora_m: diff.append("m")
    if rec_xf != ora_xf: diff.append("xf")
    # A: m=1 → low byte only matters
    if rec_m == 1 and (rec_a & 0xFF) != (ora_a & 0xFF): diff.append("Alo")
    if rec_m == 0 and rec_a != ora_a: diff.append("A")
    if rec_xf == 1 and (rec_x & 0xFF) != (ora_x & 0xFF): diff.append("Xlo")
    if rec_xf == 0 and rec_x != ora_x: diff.append("X")
    if rec_xf == 1 and (rec_y & 0xFF) != (ora_y & 0xFF): diff.append("Ylo")
    if rec_xf == 0 and rec_y != ora_y: diff.append("Y")
    diff_str = ",".join(diff) if diff else ""
    if diff and first_div_idx is None:
        first_div_idx = ri
    print(f"  {ri:>3} 0x{rec_pc:06x}  | "
          f"{fmt_h(rec_a)} {fmt_h(rec_x)} {fmt_h(rec_y)} {rec_m} {rec_xf}  | "
          f"{ora_a:04x} {ora_x:04x} {ora_y:04x} {ora_m} {ora_xf}  | {diff_str}")
    if first_div_idx is not None and ri > first_div_idx + 30:
        print("  ... (truncated 30 post-divergence)")
        break

if first_div_idx is not None:
    rb = rec[first_div_idx]
    print(f"\nFIRST DIVERGENCE at recomp block {first_div_idx}, PC=0x{int(rb['pc24'],16):06x}")
    print(f"  Look at gen_v2 source for this PC; the block above is where state was last good.")
else:
    print("\n(no divergence in this call — bug is elsewhere or in deeper branch)")
