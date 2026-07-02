#!/usr/bin/env python3
"""
cosim_align.py -- find the post-boot frame offset between the recomp A-side and
the interp816 ref (SNES_COSIM.md task 9). The two boot differently (recomp
HLE-boots the APU in one bursty run_frame; the ref runs the real IPL handshake
over many hardware frames), so their frame counters are offset. This probe
free-runs each independently, records per-frame subsystem hashes, and searches
for the offset O that maximizes WRAM agreement (A frame i  <->  B frame i+O).

If a strong offset exists, A-vs-B can be lockstepped from there and the APU/DSP
sub-hash split = the audio off-cue. If WRAM never strongly aligns, the APU-timing
divergence pervades game state and we need a narrower (port-write) comparison.

Usage: cosim_align.py --a-cmd "A.exe rom" --b-cmd "B.exe rom" [--a-frames 250 --b-frames 350]
"""
import argparse, socket, subprocess, sys, time, os, shlex

SUBS = ["cpu", "ram", "apu", "ppu", "dma", "cart", "sio", "dsp", "spc"]


class Side:
    def __init__(self, name, port, cmd):
        env = dict(os.environ)
        env["SNES_COSIM_PORT"] = str(port)
        env["SNES_COSIM_STRIDE"] = "1"
        self.proc = subprocess.Popen(shlex.split(cmd), env=env)
        self.port = port; self.name = name; self.buf = b""

    def connect(self, timeout=60):
        t0 = time.time()
        while time.time() - t0 < timeout:
            try:
                self.sock = socket.create_connection(("127.0.0.1", self.port), timeout=5)
                self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                return
            except OSError:
                if self.proc.poll() is not None:
                    sys.exit(f"{self.name} exited before connect")
                time.sleep(0.3)
        sys.exit(f"{self.name}: connect timeout")

    def _line(self):
        while b"\n" not in self.buf:
            c = self.sock.recv(4096)
            if not c: sys.exit(f"{self.name} closed")
            self.buf += c
        l, self.buf = self.buf.split(b"\n", 1)
        return l.decode(errors="replace").strip()

    def cmd(self, s):
        self.sock.sendall((s + "\n").encode()); return self._line()

    def close(self):
        try: self.sock.close()
        except OSError: pass
        if self.proc.poll() is None: self.proc.terminate()


def kv(line):
    return {k: v for k, v in (t.split("=", 1) for t in line.split() if "=" in t)}


def record(side, n):
    """Step n frames, return list of per-frame sub-hash dicts."""
    rows = []
    for _ in range(n):
        side.cmd("step 1")
        rows.append(kv(side.cmd("sub")))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--a-cmd", required=True); ap.add_argument("--b-cmd", required=True)
    ap.add_argument("--a-port", type=int, default=4500); ap.add_argument("--b-port", type=int, default=4501)
    ap.add_argument("--a-frames", type=int, default=250)
    ap.add_argument("--b-frames", type=int, default=400)
    args = ap.parse_args()

    a = Side("A", args.a_port, args.a_cmd); b = Side("B", args.b_port, args.b_cmd)
    a.connect(); b.connect()
    print(f"recording A={args.a_frames} B={args.b_frames} frames...")
    ra = record(a, args.a_frames); rb = record(b, args.b_frames)

    # search offset O: A frame i  <->  B frame i+O, maximize ram agreement
    best = (0, -1)
    for O in range(-20, args.b_frames - args.a_frames + 1):
        m = sum(1 for i in range(len(ra))
                if 0 <= i + O < len(rb) and ra[i].get("ram") == rb[i + O].get("ram"))
        if m > best[1]:
            best = (O, m)
    O, m = best
    overlap = sum(1 for i in range(len(ra)) if 0 <= i + O < len(rb))
    print(f"\nBEST WRAM offset O={O}: {m}/{overlap} frames with IDENTICAL ram hash")

    # also report best agreement per subsystem at that offset
    print("per-subsystem agreement at O:")
    for s in SUBS:
        m2 = sum(1 for i in range(len(ra))
                 if 0 <= i + O < len(rb) and ra[i].get(s) == rb[i + O].get(s))
        print(f"  {s:5} {m2}/{overlap}")

    # show a few aligned frames near the middle
    print("\nsample aligned frames (A i / B i+O): ram apu dsp spc match?")
    mid = len(ra) // 2
    for i in range(mid, min(mid + 6, len(ra))):
        j = i + O
        if not (0 <= j < len(rb)): continue
        def eq(s): return "=" if ra[i].get(s) == rb[j].get(s) else "X"
        print(f"  A{i:4}/B{j:4}  ram{eq('ram')} apu{eq('apu')} dsp{eq('dsp')} spc{eq('spc')} cpu{eq('cpu')} sio{eq('sio')}")

    a.close(); b.close()


if __name__ == "__main__":
    sys.exit(main())
