#!/usr/bin/env python3
"""Drive smw_cosim.exe to free-run N frames while it emits its per-frame WRAM
trace (SNESRECOMP_WRAM_TRACE_FILE), for the frame-boundary co-sim vs bsnes.
The cosim build parks for a coordinator, so we connect and step it; the recomp
runtime writes the trace itself (recomp_wram_trace_tick, same jsonl shape as
snesref).
Usage: trace_recomp.py <exe> <rom> <trace_out> [frames] [--apuram <apu_out>]
--apuram additionally captures the 64K SPC/APU RAM trace in the SAME run (audio
hunt), guaranteeing it reflects the identical execution as the WRAM trace."""
import socket, subprocess, sys, os, time

argv = sys.argv[1:]
def take(flag):
    if flag in argv:
        i = argv.index(flag); v = argv[i + 1]; del argv[i:i + 2]; return v
    return None
apu_out = take("--apuram")
dspreg_out = take("--dspreg")
dspout_out = take("--dspout")
exe, rom, out = argv[0], argv[1], argv[2]
frames = int(argv[3]) if len(argv) > 3 else 600
for f in (out, apu_out, dspreg_out, dspout_out):
    if f and os.path.exists(f):
        os.remove(f)
env = dict(os.environ)
env["SNES_COSIM_PORT"] = "4600"
env["SNES_COSIM_STRIDE"] = "1"
env["SNESRECOMP_WRAM_TRACE_FILE"] = out
if apu_out:
    env["SNESRECOMP_APURAM_TRACE_FILE"] = apu_out
if dspreg_out:
    env["SNESRECOMP_DSPREG_TRACE_FILE"] = dspreg_out
if dspout_out:
    env["SNESRECOMP_DSPOUT"] = dspout_out
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
extra = "".join(f" + {k} -> {v}" for k, v in
                (("apuram", apu_out), ("dspreg", dspreg_out), ("dspout", dspout_out)) if v)
print(f"stepped {frames} frames; trace -> {out}{extra}")
s.close(); p.terminate()
