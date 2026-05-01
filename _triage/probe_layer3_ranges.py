"""Run vram_write_diff over the three Layer-3 / HUD byte ranges
flagged in the original handoff (HANDOFF_VRAM_DIFFER.md / earlier
full-VRAM scan). Each range gets a separate diff so a divergence in
range A doesn't hide a divergence in range B. Skips the $C000-$FFFF
OBJ tile range that the Mario-sprite bug dominates.
"""
import argparse, json, socket, sys

p = argparse.ArgumentParser()
p.add_argument("--port", type=int, default=4377)
args = p.parse_args()

s = socket.create_connection(("127.0.0.1", args.port), timeout=8)
s.settimeout(30)
buf = [b""]


def rl():
    while b"\n" not in buf[0]:
        c = s.recv(1 << 18)
        if not c:
            break
        buf[0] += c
    nl = buf[0].find(b"\n")
    if nl < 0:
        out, buf[0] = buf[0], b""
    else:
        out, buf[0] = buf[0][:nl], buf[0][nl + 1:]
    return out.decode(errors="replace").strip()


def cmd(line):
    s.sendall((line + "\n").encode())
    return rl()


print("banner:", rl())

ranges = [
    (0x0900, 0x1DFF, "Layer-3 GFX"),
    (0x5200, 0x5FFF, "Layer-3 tilemap"),
    (0x7500, 0x84FF, "Layer-3 / HUD"),
    (0xA400, 0xB800, "ClearLayer3 fill region"),
]

for lo, hi, label in ranges:
    raw = cmd(f"vram_write_diff {lo:x} {hi:x}")
    print(f"\n=== ${lo:04X}-${hi:04X}  ({label}) ===")
    try:
        d = json.loads(raw)
    except json.JSONDecodeError:
        print("parse err:", raw[:400])
        continue
    if d.get("diverged"):
        r = d["recomp"]
        o = d["oracle"]
        print(f"  DIVERGED at idx {d['first_diff_idx']} "
              f"(matched {d['matched_pairs_before']} prior)")
        print(f"  recomp: {r['adr_byte']}={r['val']}  "
              f"func={r['func']}  f={r['f']}")
        print(f"  recomp regs: A={r['A']} X={r['X']} Y={r['Y']} "
              f"D={r['D']} DB={r['DB']} P={r['P']} m={r['m']} x={r['x']}")
        print(f"  oracle: {o['adr_byte']}={o['val']}  f={o['f']}")
        print(f"  stack:  {' -> '.join(r.get('stack', []))}")
    else:
        cnt = d.get("matched_pairs", 0)
        rec_x = d.get("recomp_exhausted", False)
        ora_x = d.get("oracle_exhausted", False)
        print(f"  CLEAN: matched {cnt} pairs  "
              f"(rec_exh={rec_x}, ora_exh={ora_x})")
