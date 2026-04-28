"""Find what PCs snes9x visits most during boot. The hot spots
are the busy-waits that drive the cycle-accuracy gap."""
from __future__ import annotations
import json, pathlib, socket, subprocess, time
from collections import Counter

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

        # Arm broad insn trace before any stepping.
        cmd(sock, f, 'emu_insn_trace_on 0x000000 0x00ffff')

        # Step until rec=GM=07.
        for _ in range(3000):
            cmd(sock, f, 'step 1')
            if int(cmd(sock, f, 'dump_ram 0x100 1')['hex'].replace(' ',''),16) == 0x07:
                break
        ef = cmd(sock, f, 'emu_frame').get('frame', '?')
        n = cmd(sock, f, 'emu_insn_trace_count').get('count', 0)
        print(f'[at rec GM=07] emu_frame={ef} insn count={n}')

        et = cmd(sock, f, 'emu_get_insn_trace').get('log', [])
        pcs = Counter(e.get('pc') for e in et)
        print(f'\nTop 30 hottest PCs in snes9x trace ({len(et)} samples):')
        for pc, count in pcs.most_common(30):
            print(f'  {pc}: {count}')
    finally:
        try: sock.close()
        except Exception: pass
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill()
        _kill()


if __name__ == '__main__':
    main()
