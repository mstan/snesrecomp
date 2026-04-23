"""Bug #8 — print the GameMode (0x100) timeline for both recomp and
oracle during boot. If recomp transitions through modes faster than
oracle, we're skipping something — and that skipped state is probably
where the $72-clearing call chain would have fired."""
from __future__ import annotations
import json, pathlib, socket, subprocess, sys, time

REPO = pathlib.Path(r'F:/Projects/SuperMarioWorldRecomp')
EXE = REPO / 'build/bin-x64-Oracle/smw.exe'
MAX_FRAMES = 250


def cmd(sock, f, line):
    sock.sendall((line + '\n').encode())
    return json.loads(f.readline())


def step1(sock, f):
    before = cmd(sock, f, 'frame').get('frame', 0)
    cmd(sock, f, 'step 1')
    deadline = time.time() + 5
    while time.time() < deadline:
        if cmd(sock, f, 'frame').get('frame', 0) > before:
            return before + 1
        time.sleep(0.01)
    return before


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

        # Collect recomp + oracle GameMode per-frame. Oracle lags; keep
        # stepping both together and record each side's current mode.
        rows = []
        prev_r = None
        prev_o = None
        for fr in range(1, MAX_FRAMES + 1):
            step1(sock, f)
            rm = int(cmd(sock, f, 'dump_ram 0x100 1')['hex'].replace(' ', ''), 16)
            r = cmd(sock, f, 'emu_read_wram 0x100 1')
            om = int(r['hex'].replace(' ', ''), 16) if r.get('ok') else -1
            if rm != prev_r or om != prev_o:
                rows.append((fr, rm, om))
                prev_r, prev_o = rm, om
        print(f'rframe    recomp_mode   oracle_mode')
        for fr, rm, om in rows:
            print(f'  {fr:4d}       0x{rm:02x}         0x{om:02x}')
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
