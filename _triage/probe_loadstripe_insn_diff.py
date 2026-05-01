"""Query both always-on insn rings for PCs inside UploadLevelTilemapHDMA
($00:871E..$00:87AC) at f=95 (recomp) / f=183 (oracle), and align by
PC + sequence within each side's call. First PC where A/X/Y/m/x diverge
identifies the v2 codegen bug.

Recomp ring: get_insn_trace pc_lo/pc_hi/limit
Oracle ring: emu_get_insn_trace pc_lo/pc_hi/from/limit

Both rings already populated since boot. Frame 95/183 is well in the past;
ring should still hold it (8M+ entries each).
"""
import argparse, json, socket, sys

p = argparse.ArgumentParser()
p.add_argument("--port", type=int, default=4377)
p.add_argument("--pc-lo", default="0x008720")
p.add_argument("--pc-hi", default="0x0087ac")
p.add_argument("--limit", type=int, default=1024)
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
    if nl < 0:
        out, buf[0] = buf[0], b""
    else:
        out, buf[0] = buf[0][:nl], buf[0][nl + 1:]
    return out.decode(errors="replace").strip()


def cmd(line):
    s.sendall((line + "\n").encode())
    return rl()


print("banner:", rl())

pc_lo = int(args.pc_lo, 16)
pc_hi = int(args.pc_hi, 16)

# Recomp side: get_insn_trace pc_lo/pc_hi
rec_raw = cmd(f"get_insn_trace pc_lo={pc_lo:x} pc_hi={pc_hi:x} limit={args.limit}")
rec_d = json.loads(rec_raw)
rec_log = rec_d.get("log", [])
print(f"recomp: total ring entries={rec_d.get('total','?')}, in-range emitted={len(rec_log)}")

# Filter to recomp frame == 95 (the specific call we care about)
rec_call = [e for e in rec_log if e.get("f") == args.rec_frame]
print(f"  at frame {args.rec_frame}: {len(rec_call)} insn hits")

# Oracle side: emu_get_insn_trace — paginate to find frame == 183 entries.
# The emu_get_insn_trace returns linear entries from idx; filter by frame.
ora_log = []
from_idx = 0
ora_total = None
while True:
    raw = cmd(f"emu_get_insn_trace from={from_idx} limit=4096 pc_lo={pc_lo:x} pc_hi={pc_hi:x}")
    d = json.loads(raw)
    ora_total = d.get("total", 0)
    chunk = d.get("log", [])
    if not chunk:
        break
    ora_log.extend(chunk)
    last_f = chunk[-1].get("f", -1)
    from_idx = chunk[-1].get("idx", from_idx + len(chunk)) + 1 if "idx" in chunk[-1] else from_idx + 4096
    # If the last in-range entry is past the ora_frame, no point continuing
    if last_f > args.ora_frame + 5:
        break
    if from_idx >= ora_total:
        break

print(f"oracle: total={ora_total}, in-range collected={len(ora_log)}")
ora_call = [e for e in ora_log if e.get("f") == args.ora_frame]
print(f"  at frame {args.ora_frame}: {len(ora_call)} insn hits")

# Align by sequence within each call
n = min(len(rec_call), len(ora_call))
print(f"\nlock-stepping first {n} insns inside the function call:")
print(f"  {'idx':>3} {'pc':>10} {'mnem':>5}  | rec A    X    Y    m x  | ora A    X    Y    m x  | diff?")
print("  " + "-" * 96)

mnems = (
    "?", "ADC", "AND", "ASL", "BCC", "BCS", "BEQ", "BIT", "BMI", "BNE",
    "BPL", "BRA", "BRK", "BRL", "BVC", "BVS", "CLC", "CLD", "CLI", "CLV",
    "CMP", "COP", "CPX", "CPY", "DEC", "DEX", "DEY", "EOR", "INC", "INX",
    "INY", "JMP", "JML", "JSL", "JSR", "LDA", "LDX", "LDY", "LSR", "MVN",
    "MVP", "NOP", "ORA", "PEA", "PEI", "PER", "PHA", "PHB", "PHD", "PHK",
    "PHP", "PHX", "PHY", "PLA", "PLB", "PLD", "PLP", "PLX", "PLY", "REP",
    "ROL", "ROR", "RTI", "RTL", "RTS", "SBC", "SEC", "SED", "SEI", "SEP",
    "STA", "STP", "STX", "STY", "STZ", "TAX", "TAY", "TCD", "TCS", "TDC",
    "TRB", "TSB", "TSC", "TSX", "TXA", "TXS", "TXY", "TYA", "TYX", "WAI",
    "WDM", "XBA", "XCE",
)


def fmt_reg(v):
    if v == "?" or v is None:
        return "????"
    return v.replace("0x", "")


first_div = None
for i in range(n):
    r = rec_call[i]
    o = ora_call[i]
    rec_pc = int(r["pc"], 16) if isinstance(r["pc"], str) else r["pc"]
    ora_pc = o["pc"] if isinstance(o["pc"], int) else int(o["pc"], 16)
    rec_mnem = mnems[r["mnem"]] if r["mnem"] < len(mnems) else "???"
    rec_a = fmt_reg(r["a"])
    rec_x = fmt_reg(r["x"])
    rec_y = fmt_reg(r["y"])
    rec_m = r["m"]
    rec_x_f = r["xf"]
    ora_a = f"{o['a']:04x}" if isinstance(o['a'], int) else fmt_reg(o['a'])
    ora_x = f"{o['x']:04x}" if isinstance(o['x'], int) else fmt_reg(o['x'])
    ora_y = f"{o['y']:04x}" if isinstance(o['y'], int) else fmt_reg(o['y'])
    ora_m = o.get("m", "?")
    ora_x_flag = o.get("xf", "?")
    diff = []
    if rec_pc != ora_pc:
        diff.append("PC")
    # In m=1 only A.low matters; in m=0 full A. Compare based on rec_m.
    rec_a_int = int(rec_a, 16) if rec_a != "????" else None
    ora_a_int = int(ora_a, 16) if ora_a != "????" else None
    if rec_a_int is not None and ora_a_int is not None:
        if rec_m == 1 and (rec_a_int & 0xFF) != (ora_a_int & 0xFF):
            diff.append("Alo")
        if rec_m == 0 and rec_a_int != ora_a_int:
            diff.append("A")
    rec_x_int = int(rec_x, 16) if rec_x != "????" else None
    ora_x_int = int(ora_x, 16) if ora_x != "????" else None
    if rec_x_int is not None and ora_x_int is not None:
        if rec_x_f == 1 and (rec_x_int & 0xFF) != (ora_x_int & 0xFF):
            diff.append("Xlo")
        if rec_x_f == 0 and rec_x_int != ora_x_int:
            diff.append("X")
    rec_y_int = int(rec_y, 16) if rec_y != "????" else None
    ora_y_int = int(ora_y, 16) if ora_y != "????" else None
    if rec_y_int is not None and ora_y_int is not None:
        if rec_x_f == 1 and (rec_y_int & 0xFF) != (ora_y_int & 0xFF):
            diff.append("Ylo")
        if rec_x_f == 0 and rec_y_int != ora_y_int:
            diff.append("Y")
    if rec_m != ora_m:
        diff.append("m")
    if rec_x_f != ora_x_flag:
        diff.append("xf")
    diff_str = ",".join(diff) if diff else ""
    if diff and first_div is None:
        first_div = i
    if i < 80 or diff:
        print(f"  {i:>3} 0x{rec_pc:06x} {rec_mnem:>5}  | {rec_a} {rec_x} {rec_y} {rec_m} {rec_x_f}  "
              f"| {ora_a} {ora_x} {ora_y} {ora_m} {ora_x_flag}  | {diff_str}")
    if first_div is not None and i > first_div + 30:
        print("  ... (truncated after 30 post-divergence insns)")
        break

def _fmt(v):
    if isinstance(v, int):
        return f"0x{v:04x}"
    return str(v)

if first_div is not None:
    r = rec_call[first_div]
    o = ora_call[first_div]
    print(f"\nFIRST DIVERGENCE at idx {first_div}, PC=0x{int(r['pc'],16):06x}, mnem={mnems[r['mnem']]}")
    print(f"  recomp: A={r['a']} X={r['x']} Y={r['y']} m={r['m']} xf={r['xf']}")
    print(f"  oracle: A={_fmt(o['a'])} X={_fmt(o['x'])} Y={_fmt(o['y'])} m={o['m']} xf={o['xf']}")
else:
    print("\n(no divergence within compared range)")
