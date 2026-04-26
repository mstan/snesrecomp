"""Disambiguate emu's s_watch_frame semantics.

Hypothesis: each `emu_step 1` advances s_watch_frame by exactly 1
(per snes9x_bridge_run_frame's s_watch_frame++ call). Trace
attribution for OAM bytes shows writes at f=296 when the GM=07
sync stops at ~204 emu_steps. Either the count of emu_steps is
higher than I think, or s_watch_frame increments differently.

This probe explicitly counts emu_step calls and queries
emu_frame after each batch to see the relationship.
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

        # Initial state.
        r = cmd(sock, f, 'emu_frame')
        print(f'[initial] emu_frame = {r.get("frame")}')

        # Step 1, query.
        cmd(sock, f, 'emu_step 1')
        r = cmd(sock, f, 'emu_frame')
        print(f'[after 1 step] emu_frame = {r.get("frame")}')

        # Step 9 more (total 10).
        cmd(sock, f, 'emu_step 9')
        r = cmd(sock, f, 'emu_frame')
        print(f'[after 10 steps] emu_frame = {r.get("frame")}')

        # Step until GM=07; count emu_steps, query emu_frame at end.
        emu_steps = 10
        for _ in range(3000):
            cmd(sock, f, 'emu_step 1'); emu_steps += 1
            gm_r = cmd(sock, f, 'emu_read_wram 0x100 1')['hex'].replace(' ', '')
            if int(gm_r, 16) == 0x07: break
        r = cmd(sock, f, 'emu_frame')
        print(f'[at GM=07 first sighting] emu_steps issued = {emu_steps}, '
              f'emu_frame = {r.get("frame")}')

        # If emu_frame == emu_steps, semantics match assumption.
        # If emu_frame > emu_steps, retro_run cycles multiple frames per call.
    finally:
        try: sock.close()
        except Exception: pass
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill()
        _kill()


if __name__ == '__main__':
    main()
