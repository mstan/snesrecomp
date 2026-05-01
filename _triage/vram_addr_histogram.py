"""Histogram of byte-VRAM addresses currently in the recomp ring."""
import argparse, json, socket, sys
from collections import Counter

p = argparse.ArgumentParser()
p.add_argument("--port", type=int, default=4377)
args = p.parse_args()

s = socket.create_connection(("127.0.0.1", args.port), timeout=8)
s.settimeout(60)
buf = [b""]
def recv_line():
    while b"\n" not in buf[0]:
        c = s.recv(1 << 20)
        if not c:
            break
        buf[0] += c
    nl = buf[0].find(b"\n")
    if nl < 0:
        out, buf[0] = buf[0], b""
    else:
        out, buf[0] = buf[0][:nl], buf[0][nl+1:]
    return out.decode(errors="replace").strip()
def cmd(line):
    s.sendall((line+"\n").encode())
    return recv_line()
print("banner:", recv_line())

print("\n--- recomp ring ---")
raw = cmd("get_vram_trace nostack")
d = json.loads(raw)
log = d["log"]
print(f"entries={d.get('entries')}, returned={len(log)}")
hist = Counter()
for e in log:
    a = int(e["adr_byte"], 16)
    hist[a & 0xFF00] += 1
print("Top byte-VRAM pages (recomp):")
for page, n in hist.most_common(20):
    print(f"  ${page:04X}-${page|0xFF:04X}  {n:6d}  ({n*100//len(log) if log else 0}%)")

# Also page-byte range $A400-$B8FF check
in_range = sum(1 for e in log if 0xA400 <= int(e["adr_byte"], 16) <= 0xB8FF)
print(f"\nrecomp writes in $A400-$B8FF: {in_range}")

print("\n--- oracle ring ---")
raw = cmd("get_oracle_vram_trace")
d = json.loads(raw)
log = d["log"]
print(f"entries={d.get('entries')}, returned={len(log)}")
hist = Counter()
for e in log:
    a = int(e["adr_byte"], 16)
    hist[a & 0xFF00] += 1
print("Top byte-VRAM pages (oracle):")
for page, n in hist.most_common(20):
    print(f"  ${page:04X}-${page|0xFF:04X}  {n:6d}  ({n*100//len(log) if log else 0}%)")

in_range = sum(1 for e in log if 0xA400 <= int(e["adr_byte"], 16) <= 0xB8FF)
print(f"\noracle writes in $A400-$B8FF: {in_range}")
