"""After GM=07 sync, step recomp 200 more frames and re-query the
always-on trace for $03E8 writes. If a write appears, the
divergence at GM=07 entry is a TIMING shift (recomp catches up).
If not, recomp genuinely never reaches $01:9DAE-equivalent."""
from __future__ import annotations
import json, pathlib, socket, subprocess, time

REPO = pathlib.Path(__file__).parent.parent.parent.parent
EXE = REPO / 'build' / 'bin-x64-Oracle' / 'smw.exe'
PORT = 4377


def _kill():
    subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def cmd(sock, f, line):
    sock.sendall((line + '\n').encode())
    return json.loads(f.readline())


def main():
    _kill(); time.sleep(0.5)
    proc = subprocess.Popen([str(EXE), '--paused'], cwd=str(REPO),
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    try:
        sock = socket.socket()
        for _ in range(60):
            try:
                sock.connect(('127.0.0.1', PORT)); break
            except (ConnectionRefusedError, OSError):
                time.sleep(0.2)
        f = sock.makefile('r'); f.readline()
        cmd(sock, f, 'pause')

        for _ in range(3000):
            cmd(sock, f, 'step 1')
            r = cmd(sock, f, 'dump_ram 0x100 1')['hex'].replace(' ', '')
            if int(r, 16) == 0x07: break
        print('rec at GM=07. Stepping +200 more...')
        for _ in range(200):
            cmd(sock, f, 'step 1')

        gm = cmd(sock, f, 'dump_ram 0x100 1')['hex'].replace(' ', '')
        print(f'after +200, GM=$0100={gm}')

        for a in [0x3e8, 0x3eb, 0x3ec, 0x3ef, 0x41e, 0x49a, 0x49b,
                  0x14a2, 0x2142, 0x2143]:
            r = cmd(sock, f, f'wram_writes_at {a:x} 0 999999 32')
            v = cmd(sock, f, f'dump_ram 0x{a:x} 1')['hex'].replace(' ', '')
            print(f'  ${a:04x}: cur=${v} writes={len(r.get("matches",[]))}')
            for e in r.get('matches', []):
                print(f'    f={e["f"]} val={e["val"]} func={e["func"]} parent={e["parent"]}')
    finally:
        try: sock.close()
        except Exception: pass
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill()
        _kill()


if __name__ == '__main__':
    main()
