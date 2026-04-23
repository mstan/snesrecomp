"""Count RunPlayerBlockCode_EB77 hits on recomp during boot."""
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
    subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.5)
    p = subprocess.Popen([str(EXE), '--paused'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, cwd=str(REPO))
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
        for start in range(0, 220, 30):
            for q in ['RunPlayerBlockCode_EB77', 'RunPlayerBlockCode',
                      'HandleHorizontalSubScreenCrossingForCurrentObject',
                      'SetMap16HighByteForCurrentObject',
                      'RunPlayerBlockCode_00F44D']:
                r = cmd(s, f, f'get_call_trace from={start} to={start+35} contains={q}')
                for e in r.get('log', []):
                    fn = e.get('func', '')
                    if q in fn:
                        counts[fn] += 1; firsts.setdefault(fn, e.get('f'))
        for fn, c in sorted(counts.items(), key=lambda x: -x[1])[:20]:
            print(f'{c:5d} f{firsts[fn]} {fn}')
        return 0
    finally:
        try: s.close()
        except Exception: pass
        p.terminate()
        try: p.wait(timeout=5)
        except subprocess.TimeoutExpired: p.kill()
        subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

if __name__ == '__main__':
    sys.exit(main())
