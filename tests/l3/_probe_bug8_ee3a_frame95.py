"""Check ALL calls in frame 95 on recomp containing EE3A."""
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
        for _ in range(100): step1(s, f)
        # Dump everything at frame 95.
        r = cmd(s, f, 'get_call_trace from=95 to=95')
        log = r.get('log', [])
        print(f'frame 95 call entries: {len(log)}')
        # Unique names only.
        names = {}
        for e in log:
            fn = e.get('func', '')
            if fn not in names:
                names[fn] = (e.get('d'), e.get('parent'))
        print('\nunique function names at frame 95:')
        for fn, (d, par) in sorted(names.items(), key=lambda x: x[1][0]):
            print(f'  d{d:2} {fn} <- {par}')
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
