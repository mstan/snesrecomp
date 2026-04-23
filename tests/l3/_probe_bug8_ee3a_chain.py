"""Check if RunPlayerBlockCode_00EE3A fires on recomp during boot.
Oracle reaches $EF6B (STZ PlayerInAir) via CODE_00EE3A -> fall through
to $EE85 -> $EED1 -> $EEE1 -> $EF60 -> $EF6B. If EE3A doesn't fire on
recomp, that's the break."""
from __future__ import annotations
import json, pathlib, socket, subprocess, sys, time
REPO = pathlib.Path(r'F:/Projects/SuperMarioWorldRecomp')
EXE = REPO / 'build/bin-x64-Oracle/smw.exe'
def cmd(s, f, l):
    s.sendall((l + '\n').encode()); return json.loads(f.readline())
def step1(s, f):
    b = cmd(s, f, 'frame').get('frame', 0); cmd(s, f, 'step 1')
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
        cmd(s, f, 'trace_calls_reset'); cmd(s, f, 'trace_calls')
        for _ in range(210): step1(s, f)
        from collections import Counter
        counts = Counter(); firsts = {}
        keys = [
            'RunPlayerBlockCode_00EE3A',
            'RunPlayerBlockCode_00EE85',
            'RunPlayerBlockCode_00EED1',
            'RunPlayerBlockCode_00EEE1',
            'RunPlayerBlockCode_CheckIfBlockWasHit',
            'CheckIfBlockWasHit',
        ]
        for start in range(0, 220, 30):
            for q in keys:
                r = cmd(s, f, f'get_call_trace from={start} to={start+35} contains={q}')
                for e in r.get('log', []):
                    fn = e.get('func', '')
                    if q in fn:
                        counts[fn] += 1; firsts.setdefault(fn, e.get('f'))
        print('hits:')
        for fn, c in sorted(counts.items(), key=lambda x: -x[1]):
            print(f'  {c:4d}  f{firsts[fn]}  {fn}')
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
