"""Use recomp's Tier 4 `trace_insn` to capture exact PC + register state
through EB77's body at frame 95. Compare to oracle's insn trace —
find where X register diverges (oracle has X=8 at $ED4A, recomp has
v1=0)."""
from __future__ import annotations
import json, pathlib, socket, subprocess, sys, time

REPO = pathlib.Path(r'F:/Projects/SuperMarioWorldRecomp')
EXE = REPO / 'build/bin-x64-Oracle/smw.exe'


def cmd(s, f, l):
    s.sendall((l + '\n').encode())
    return json.loads(f.readline())


def step1(s, f):
    b = cmd(s, f, 'frame').get('frame', 0)
    cmd(s, f, 'step 1')
    dl = time.time() + 5
    while time.time() < dl:
        if cmd(s, f, 'frame').get('frame', 0) > b: return b + 1
        time.sleep(0.01)


def main():
    subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.5)
    p = subprocess.Popen([str(EXE), '--paused'],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, cwd=str(REPO))
    try:
        s = socket.socket()
        for _ in range(50):
            try: s.connect(('127.0.0.1', 4377)); break
            except (ConnectionRefusedError, OSError): time.sleep(0.2)
        f = s.makefile('r'); f.readline()

        # Advance to counter=95 (EB77 will run during the next step).
        for _ in range(95):
            step1(s, f)

        cmd(s, f, 'trace_insn_reset')
        cmd(s, f, 'trace_insn')
        step1(s, f)

        # Pull insn trace in EB77 body range.
        r = cmd(s, f, 'get_insn_trace pc_lo=0xeb77 pc_hi=0xeeff limit=400')
        log = r.get('log', [])
        print(f'recomp insn trace in $EB77-$EEFF range: {len(log)}')
        for e in log:
            print(f'  pc={e["pc"]} mnem={e["mnem"]:3} '
                  f'a={e["a"]:8} x={e["x"]:8} y={e["y"]:8} m={e["m"]} xf={e["xf"]}')
        return 0
    finally:
        try: s.close()
        except Exception: pass
        p.terminate()
        try: p.wait(timeout=5)
        except subprocess.TimeoutExpired: p.kill()
        subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


if __name__ == '__main__':
    sys.exit(main())
