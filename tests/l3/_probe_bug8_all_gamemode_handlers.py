"""Count every GameMode_* handler entry during recomp boot. If
GameMode04_PrepareTitleScreen has 0 hits, mode 0x04 wasn't dispatched
even though $100 went 0x03 -> 0x04 -> 0x05."""
from __future__ import annotations
import json, pathlib, socket, subprocess, sys, time

REPO = pathlib.Path(r'F:/Projects/SuperMarioWorldRecomp')
EXE = REPO / 'build/bin-x64-Oracle/smw.exe'


def cmd(sock, f, line):
    sock.sendall((line + '\n').encode())
    return json.loads(f.readline())


def step1(sock, f):
    b = cmd(sock, f, 'frame').get('frame', 0)
    cmd(sock, f, 'step 1')
    deadline = time.time() + 5
    while time.time() < deadline:
        if cmd(sock, f, 'frame').get('frame', 0) > b: return b + 1
        time.sleep(0.01)


def main():
    subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.5)
    proc = subprocess.Popen([str(EXE), '--paused'],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, cwd=str(REPO))
    try:
        sock = socket.socket()
        for _ in range(50):
            try: sock.connect(('127.0.0.1', 4377)); break
            except (ConnectionRefusedError, OSError): time.sleep(0.2)
        f = sock.makefile('r')
        f.readline()
        cmd(sock, f, 'trace_calls_reset')
        cmd(sock, f, 'trace_calls')
        # Advance to frame 201.
        for fr in range(201):
            step1(sock, f)
        # Use the `contains=` filter to tighten output to just
        # GameMode/InitAndMainLoop entries. Also sweep with different
        # `from=` windows to break past the JSON-buffer truncation.
        from collections import Counter
        counts = Counter()
        first_f = {}
        last_f = {}
        total = 0
        for start in range(0, 220, 40):
            r = cmd(sock, f, f'get_call_trace from={start} to={start + 45} contains=GameMode')
            entries = r.get('log', [])
            total += len(entries)
            for e in entries:
                fn = e.get('func', '')
                counts[fn] += 1
                first_f.setdefault(fn, e.get('f'))
                last_f[fn] = e.get('f')
        for start in range(0, 220, 40):
            r = cmd(sock, f, f'get_call_trace from={start} to={start + 45} contains=InitAndMainLoop')
            entries = r.get('log', [])
            total += len(entries)
            for e in entries:
                fn = e.get('func', '')
                counts[fn] += 1
                first_f.setdefault(fn, e.get('f'))
                last_f[fn] = e.get('f')
        print(f'total (across windows): {total}')
        print(f'\nGameMode* / InitAndMainLoop* hits:')
        for fn, c in sorted(counts.items(), key=lambda x: -x[1]):
            print(f'  {c:5d}  f{first_f[fn]}-{last_f[fn]}  {fn}')
        return 0
    finally:
        try: sock.close()
        except Exception: pass
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill()
        subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


if __name__ == '__main__':
    sys.exit(main())
