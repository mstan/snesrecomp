#!/usr/bin/env python3
"""Step a single cosim side to frame N and dump framebuffers along the way.
Usage: fbdump.py <port> <tag> <frames...>   (launch the exe separately with
SNES_COSIM_PORT=<port>; connects, steps to each frame, dumpfb after each)."""
import socket, sys, os

port = int(sys.argv[1]); tag = sys.argv[2]
frames = sorted(int(f) for f in sys.argv[3:])
s = socket.create_connection(("127.0.0.1", port), timeout=300)
buf = b""
def line():
    global buf
    while b"\n" not in buf:
        c = s.recv(4096)
        if not c: raise EOFError
        buf += c
    l, buf = buf.split(b"\n", 1)
    return l.decode(errors="replace").strip()
def cmd(c):
    s.sendall((c + "\n").encode())
    return line()

here = os.path.dirname(os.path.abspath(__file__))
# MSYS python yields POSIX paths ("/f/...") that Windows fopen can't take.
if len(here) > 2 and here[0] == "/" and len(here.split("/")) > 2 and len(here.split("/")[1]) == 1:
    here = here.split("/")[1].upper() + ":/" + "/".join(here.split("/")[2:])
cur = 0
for f in frames:
    if f > cur:
        r = cmd(f"step {f - cur}")
        cur = f
    out = os.path.join(here, f"fb_{tag}_{f:04d}.bmp").replace("\\", "/")
    print(f, cmd(f"dumpfb {out}"))
print("sub:", cmd("sub"))
print("dev:", cmd("dev"))
s.close()
