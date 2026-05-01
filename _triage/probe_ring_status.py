"""Probe ring populations to diagnose why recomp insn ring is empty."""
import socket, sys

s = socket.create_connection(("127.0.0.1", 4377), timeout=8)
s.settimeout(20)
buf = [b""]


def rl():
    while b"\n" not in buf[0]:
        c = s.recv(1 << 18)
        if not c:
            break
        buf[0] += c
    nl = buf[0].find(b"\n")
    out, buf[0] = buf[0][:nl], buf[0][nl + 1:]
    return out.decode(errors="replace")


def cmd(line):
    s.sendall((line + "\n").encode())
    return rl()


print("banner:", rl())
print("frame:", cmd("frame"))
print("block trace:", cmd("get_block_trace limit=2")[:300])
print("insn trace (rec):", cmd("get_insn_trace limit=2")[:300])
print("emu_insn_trace_count:", cmd("emu_insn_trace_count"))
print("after trace_insn arm:", cmd("trace_insn"))
print("insn trace (rec) after arm:", cmd("get_insn_trace limit=2")[:300])
