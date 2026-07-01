#!/usr/bin/env python3
"""Drive smw_cosim.exe to free-run N frames while it emits its per-frame WRAM
trace (SNESRECOMP_WRAM_TRACE_FILE), for the frame-boundary co-sim vs bsnes.
The cosim build parks for a coordinator, so we connect and step it; the recomp
runtime writes the trace itself (recomp_wram_trace_tick, same jsonl shape as
snesref). Usage: trace_recomp.py <exe> <rom> <trace_out> [frames]"""
import socket, subprocess, sys, os, time

exe, rom, out = sys.argv[1], sys.argv[2], sys.argv[3]
frames = int(sys.argv[4]) if len(sys.argv) > 4 else 600
if os.path.exists(out):
    os.remove(out)
env = dict(os.environ)
env["SNES_COSIM_PORT"] = "4600"
env["SNES_COSIM_STRIDE"] = "1"
env["SNESRECOMP_WRAM_TRACE_FILE"] = out
p = subprocess.Popen([exe, rom], env=env)
s = None
for _ in range(80):
    try:
        s = socket.create_connection(("127.0.0.1", 4600), timeout=5); break
    except OSError:
        if p.poll() is not None: sys.exit("recomp exited before connect")
        time.sleep(0.3)
if not s: sys.exit("connect timeout")
buf = b""
def line():
    global buf
    while b"\n" not in buf: buf += s.recv(4096)
    l, buf = buf.split(b"\n", 1); return l.decode()
def cmd(c): s.sendall((c + "\n").encode()); return line()
for i in range(frames):
    cmd("step 1")
print(f"stepped {frames} frames; trace -> {out}")
s.close(); p.terminate()
