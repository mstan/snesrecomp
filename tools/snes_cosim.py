#!/usr/bin/env python3
"""
snes_cosim.py -- coordinator for the SNES differential co-simulation
(SNES_COSIM.md). Drives two deterministic instances (A = code under test,
B = reference) in frame-keyed checkpoint lockstep on the guest master clock,
compares full-state chain hashes, and reports the FIRST divergence with a
per-subsystem sub-hash diff + CPU/device field diff + a window of both rings.

DEV/DIAGNOSTICS ONLY. Talks to the `#ifdef SNES_COSIM` TCP server in cosim.c.

Two ways to supply each side:
  --a-cmd "<shell>"   launch A (env SNES_COSIM_PORT/STRIDE injected); else
  --a-port N          attach to an already-launched A (default 4500)
  (likewise --b-cmd / --b-port, default 4501)

Gate runs (see SNES_COSIM.md "Validation gates"):
  Gate 1  A-vs-A: point both at the SAME build (recomp).  MUST be 0 divergence.
  Gate 2  B-vs-B: both the ref build.                      MUST be 0 divergence.
  Gate 3  --inject ram:ADDR:VAL (or reg:NAME:VAL) --inject-at CP
          fault one side after CP; tool MUST halt ~CP and name the subsystem.
  Gate 4  --audit N (env SNES_COSIM_AUDIT=N): periodic hash-vs-byte self-check.
Only after 1-4 pass do you trust an A-vs-B result.
"""
import argparse, socket, subprocess, sys, time, os, shlex

SUBS = ["cpu", "ram", "apu", "ppu", "dma", "cart", "sio", "dsp", "spc", "pace"]
COMPARED = ["cpu", "ram", "apu", "ppu", "dma", "cart", "sio"]  # in `combined`


class Side:
    def __init__(self, name, port, cmd, stride, audit, extra_env=None):
        self.name = name
        self.port = port
        self.proc = None
        if cmd:
            env = dict(os.environ)
            env["SNES_COSIM_PORT"] = str(port)
            env["SNES_COSIM_STRIDE"] = str(stride)
            if audit:
                env["SNES_COSIM_AUDIT"] = str(audit)
            for kv_ in (extra_env or []):
                k, _, v = kv_.partition("=")
                env[k] = v
            # headless + deterministic: no host audio sink / worker threads.
            env.setdefault("SNESRECOMP_HEADLESS", "1")
            env.setdefault("SNESRECOMP_NO_AUDIO", "1")
            # shell=False + shlex so a forward-slash Windows exe path (F:/...) is
            # handed straight to CreateProcess (cmd.exe mangles forward slashes).
            self.proc = subprocess.Popen(shlex.split(cmd), env=env)
        self.sock = None

    def connect(self, timeout=60.0):
        t0 = time.time()
        while time.time() - t0 < timeout:
            try:
                s = socket.create_connection(("127.0.0.1", self.port), timeout=5)
                s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                self.sock = s
                self.buf = b""
                return
            except OSError:
                if self.proc and self.proc.poll() is not None:
                    die(f"{self.name}: process exited (code {self.proc.returncode}) before connect")
                time.sleep(0.3)
        die(f"{self.name}: could not connect to port {self.port} within {timeout}s")

    def _line(self):
        while b"\n" not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                die(f"{self.name}: connection closed unexpectedly")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return line.decode(errors="replace").strip()

    def cmd(self, line):
        self.sock.sendall((line + "\n").encode())
        return self._line()

    def cmd_multi(self, line, end):
        self.sock.sendall((line + "\n").encode())
        out = []
        while True:
            l = self._line()
            if l == end:
                break
            out.append(l)
        return out

    def close(self):
        try:
            if self.sock:
                self.sock.close()
        except OSError:
            pass
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()


def die(msg):
    print(f"FATAL: {msg}", file=sys.stderr)
    sys.exit(2)


def kv(line):
    """Parse 'k=v k=v ...' into a dict. Tolerates a leading bare word."""
    d = {}
    for tok in line.split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            d[k] = v
    return d


def step_chain(side):
    """Advance one checkpoint; return (cp:int, chain:str). Fatal if unparseable
    (the PSX lesson: a None==None compare makes the tool silently blind)."""
    r = side.cmd("step 1")
    d = kv(r)
    if "chain" not in d or "cp" not in d:
        die(f"{side.name}: unparseable step reply {r!r} (tool would be BLIND)")
    return int(d["cp"]), d["chain"]


def step_subset(side, keys):
    """Advance one checkpoint; return (cp, signature over only `keys`). Lets a
    variable-under-test experiment ignore subsystems KNOWN to differ (e.g. the
    APU when comparing synthetic-vs-accurate pacing) and halt only when the
    difference reaches the compared subsystems (e.g. cpu/ram = game behaviour)."""
    r = side.cmd("step 1")
    d = kv(r)
    if "cp" not in d:
        die(f"{side.name}: unparseable step reply {r!r}")
    s = kv(side.cmd("sub"))
    sig = "|".join(s.get(k, "?") for k in keys)
    if "?" in sig.split("|"):
        die(f"{side.name}: sub missing a compared key {keys}: {s}")
    return int(d["cp"]), sig


def _host_path(p):
    """MSYS python cwd is POSIX (/f/...); the guest exe's fopen needs F:/..."""
    p = os.path.abspath(p).replace("\\", "/")
    parts = p.split("/")
    if p.startswith("/") and len(parts) > 2 and len(parts[1]) == 1:
        return parts[1].upper() + ":/" + "/".join(parts[2:])
    return p


def parse_ram_mask(spec):
    """'0x02FF,0x1F00-0x1F0F' -> set of WRAM offsets."""
    mask = set()
    for tok in spec.split(","):
        tok = tok.strip()
        if not tok:
            continue
        if "-" in tok:
            lo, hi = tok.split("-", 1)
            mask.update(range(int(lo, 0), int(hi, 0) + 1))
        else:
            mask.add(int(tok, 0))
    return mask


def ram_divergence_masked(a, b, keys, siga, sigb, mask):
    """True iff the divergence is confined to `ram` AND a byte-level dumpram
    diff shows every differing WRAM offset inside `mask`. Never a blind
    waiver: the byte diff is re-verified on every occurrence. Returns
    (masked: bool, diff_offsets: list)."""
    split = [k for k, va, vb in zip(keys, siga.split("|"), sigb.split("|"))
             if va != vb]
    if split != ["ram"]:
        return False, []
    pa = _host_path("cosim_maskchk_A.bin")
    pb = _host_path("cosim_maskchk_B.bin")
    ra = a.cmd(f"dumpram {pa}")
    rb = b.cmd(f"dumpram {pb}")
    if not (ra.startswith("ok") and rb.startswith("ok")):
        return False, []
    da = open("cosim_maskchk_A.bin", "rb").read()
    db = open("cosim_maskchk_B.bin", "rb").read()
    diffs = [i for i in range(min(len(da), len(db))) if da[i] != db[i]]
    return (len(diffs) > 0 and all(i in mask for i in diffs)), diffs


def report_divergence(a, b, cp):
    print(f"\n=== FIRST DIVERGENCE at checkpoint {cp} ===")
    sa, sb = kv(a.cmd("sub")), kv(b.cmd("sub"))
    print(f"  {'sub':6} {'A='+a.name:24} {'B='+b.name:24}")
    split = []
    for s in SUBS:
        va, vb = sa.get(s, "?"), sb.get(s, "?")
        mark = "  " if va == vb else "<-"
        if va != vb and s in COMPARED:
            split.append(s)
        print(f"  {s:6} {va:24} {vb:24} {mark}")
    print(f"  compared subsystems that split: {split or '(none — check ruler/pc currency)'}")
    print(f"\n  clk/label:  A cyc={sa.get('cyc')} mcyc={sa.get('mcyc')} pc={sa.get('pc')}"
          f"   B cyc={sb.get('cyc')} mcyc={sb.get('mcyc')} pc={sb.get('pc')}")
    if "cpu" in split:
        print("\n  CPU field diff:")
        print(f"    A: {a.cmd('cpu')}")
        print(f"    B: {b.cmd('cpu')}")
    if split and split != ["cpu"] and any(s in split for s in ("apu", "ppu", "dma", "sio")):
        print("\n  DEV field diff:")
        print(f"    A: {a.cmd('dev')}")
        print(f"    B: {b.cmd('dev')}")
    print("\n  window (last checkpoints, A then B):")
    for l in a.cmd_multi("window 12", "win-end"):
        print(f"    A {l}")
    for l in b.cmd_multi("window 12", "win-end"):
        print(f"    B {l}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--a-cmd"); ap.add_argument("--b-cmd")
    ap.add_argument("--a-port", type=int, default=4500)
    ap.add_argument("--b-port", type=int, default=4501)
    ap.add_argument("--stride", type=int, default=1, help="checkpoint every N frames")
    ap.add_argument("--max", type=int, default=100000, help="max checkpoints")
    ap.add_argument("--audit", type=int, default=0, help="hash-vs-byte audit period (gate 4)")
    ap.add_argument("--inject", help="ram:ADDR:VAL or reg:NAME:VAL (gate 3), applied to B")
    ap.add_argument("--inject-at", type=int, default=0, help="inject after this checkpoint")
    ap.add_argument("--a-env", action="append", default=[], help="extra env KEY=VAL for A (repeatable)")
    ap.add_argument("--b-env", action="append", default=[], help="extra env KEY=VAL for B (repeatable)")
    ap.add_argument("--compare", help="comma list of subsystems to compare (default: full chain). "
                    "e.g. --compare cpu,ram,ppu,dma,cart ignores apu/dsp/spc/sio "
                    "(the known variable-under-test) and halts when it reaches game state")
    ap.add_argument("--transient-grace", type=int, default=0,
                    help="on a divergence, step up to K more checkpoints; if the "
                    "per-checkpoint hashes match again, log it as TRANSIENT and "
                    "continue instead of halting. For cross-execution-model gates "
                    "(bounced vs interpreted) where cycle-model skew can shift a "
                    "boot poll by one frame and leave dead-state residue (stack "
                    "bytes above S) that reconverges. Only meaningful with "
                    "--compare (per-checkpoint subset hashes; the full chain "
                    "accumulates history and never reconverges). Localize any "
                    "reported transient once with the dumpram command before "
                    "trusting it. 0 = halt on first divergence (default).")
    ap.add_argument("--ram-mask",
                    help="comma list of WRAM offsets/ranges (hex ok: 0x02FF, "
                    "0x1F00-0x1F0F) asserted DEAD (e.g. interrupt-push residue "
                    "above a returned stack pointer, whose PB byte differs "
                    "between execution models with coarse PB currency). When a "
                    "divergence's split is EXACTLY {ram}, both sides are "
                    "dumpram'd and byte-diffed; only if EVERY differing offset "
                    "is inside the mask is it logged MASKED and the run "
                    "continues — verified per occurrence, never a blind "
                    "waiver. Requires --compare (needs per-key sigs).")
    args = ap.parse_args()
    keys = args.compare.split(",") if args.compare else None

    a = Side("A", args.a_port, args.a_cmd, args.stride, args.audit, args.a_env)
    b = Side("B", args.b_port, args.b_cmd, args.stride, args.audit, args.b_env)
    a.connect(); b.connect()
    print(f"connected: A:{args.a_port} B:{args.b_port}  stride={args.stride} frames")

    if keys:
        print(f"comparing ONLY subsystems: {keys}")
    injected = args.inject is None
    transients = []
    masked_cps = []
    ram_mask = parse_ram_mask(args.ram_mask) if args.ram_mask else None
    if ram_mask and not keys:
        die("--ram-mask requires --compare (per-key signatures)")
    t0 = time.time()
    i = 0
    while i < args.max:
        i += 1
        if keys:
            cpa, cha = step_subset(a, keys); cpb, chb = step_subset(b, keys)
        else:
            cpa, cha = step_chain(a); cpb, chb = step_chain(b)
        if cpa != cpb:
            print(f"cp index skew: A={cpa} B={cpb} (harness bug — should never happen)")
        if not injected and cpa >= args.inject_at:
            kind, addr, val = args.inject.split(":")
            r = b.cmd(f"inject {kind} {addr} {val}")
            print(f"injected {args.inject} into B at cp {cpa}: {r}")
            injected = True
        if cha != chb:
            if ram_mask:
                ok_mask, offs = ram_divergence_masked(a, b, keys, cha, chb, ram_mask)
                if ok_mask:
                    masked_cps.append(cpa)
                    if len(masked_cps) <= 8 or len(masked_cps) % 500 == 0:
                        print(f"  MASKED ram divergence at cp {cpa} "
                              f"(bytes: {['0x%04X' % o for o in offs]}, "
                              f"occurrence #{len(masked_cps)})")
                    continue
            first_cp, reconverged = cpa, False
            for g in range(1, args.transient_grace + 1):
                if i >= args.max:
                    break
                i += 1
                if keys:
                    cpa, cha = step_subset(a, keys); cpb, chb = step_subset(b, keys)
                else:
                    cpa, cha = step_chain(a); cpb, chb = step_chain(b)
                if cha == chb:
                    transients.append((first_cp, g))
                    print(f"  TRANSIENT divergence at cp {first_cp} — "
                          f"reconverged after {g} checkpoint(s)")
                    reconverged = True
                    break
            if not reconverged:
                report_divergence(a, b, cpa)
                print(f"\nhalted at cp {cpa} after {time.time()-t0:.1f}s"
                      + (f" (first divergence at cp {first_cp}, no reconvergence "
                         f"within grace {args.transient_grace})"
                         if args.transient_grace else ""))
                a.close(); b.close()
                return 1
        if i % 200 == 0:
            print(f"  cp {cpa} chain match ({time.time()-t0:.0f}s)")
    notes = []
    if transients:
        notes.append(f"{len(transients)} transient(s) at {[cp for cp, _ in transients]}"
                     f" (localize once via dumpram)")
    if masked_cps:
        notes.append(f"{len(masked_cps)} MASKED ram divergence(s) "
                     f"(byte-verified within --ram-mask each time)")
    if notes:
        print(f"no UNMASKED PERSISTENT divergence in {args.max} checkpoints "
              f"({time.time()-t0:.0f}s); " + "; ".join(notes) + " — GATE PASS")
    else:
        print(f"no divergence in {args.max} checkpoints ({time.time()-t0:.0f}s) — GATE PASS")
    a.close(); b.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
