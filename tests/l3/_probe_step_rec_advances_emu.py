"""Check whether `step N` on the recomp side advances snes9x's
internal frame counter as a side-effect.

If yes: the boot-GM probe's "emu reaches GM=07 in 204 iterations"
is actually "204 iterations starting from a non-zero emu_frame
because the rec sync loop pre-advanced emu by some amount."
"""
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

        r = cmd(sock, f, 'emu_frame')
        print(f'[initial] emu_frame = {r.get("frame")}')

        # Step ONLY the recomp side 100 times, then check emu_frame.
        for _ in range(100):
            cmd(sock, f, 'step 1')
        r = cmd(sock, f, 'emu_frame')
        print(f'[after 100 rec steps, no emu steps] emu_frame = {r.get("frame")}')

        # Now step ONLY rec until rec sees GM=07.
        rec_steps = 100
        for _ in range(3000):
            cmd(sock, f, 'step 1'); rec_steps += 1
            gm = cmd(sock, f, 'dump_ram 0x100 1')['hex'].replace(' ', '')
            if int(gm, 16) == 0x07: break
        r = cmd(sock, f, 'emu_frame')
        print(f'[after rec reaches GM=07 ({rec_steps} rec steps), 0 emu steps] '
              f'emu_frame = {r.get("frame")}')

        # Now step ONLY emu until emu sees GM=07.
        emu_steps = 0
        for _ in range(3000):
            cmd(sock, f, 'emu_step 1'); emu_steps += 1
            gm = cmd(sock, f, 'emu_read_wram 0x100 1')['hex'].replace(' ', '')
            if int(gm, 16) == 0x07: break
        r = cmd(sock, f, 'emu_frame')
        print(f'[after emu reaches GM=07 ({emu_steps} emu steps from above point)] '
              f'emu_frame = {r.get("frame")}')
    finally:
        try: sock.close()
        except Exception: pass
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill()
        _kill()


if __name__ == '__main__':
    main()
