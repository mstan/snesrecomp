"""Issue B with new lockstep-aware tooling.

Strategy: sync past GM=07 into NMI-enabled gameplay (where 1 step
= 1 logical frame on BOTH sides cleanly). Compare Mario's
trajectory in lockstep. Boot residue exists but doesn't affect
in-gameplay collision physics.

Concretely: step both sides past GM=07 by N=100 frames so we're
past GM=07 init into actual gameplay. Then sample Mario's
X/Y/speed/pose every step for the next 200 frames. If Mario's
delta-Y per frame matches between rec and emu, collision is
equivalent. If diverges, we have a Y-write attribution target.
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


def _read_byte(sock, f, addr, side):
    c = (f'dump_ram 0x{addr:x} 1' if side == 'rec'
         else f'emu_read_wram 0x{addr:x} 1')
    r = cmd(sock, f, c)
    h = r.get('hex', '').replace(' ', '')
    return int(h, 16) if h else -1


def _read_word(sock, f, addr, side):
    return (_read_byte(sock, f, addr + 1, side) << 8) | _read_byte(sock, f, addr, side)


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

        # Step both to GM=07 (auto-lockstep).
        for _ in range(3000):
            cmd(sock, f, 'step 1')
            if _read_byte(sock, f, 0x100, 'rec') == 0x07: break

        # Step further into gameplay.
        for _ in range(50):
            cmd(sock, f, 'step 1')

        # Confirm both sides in gameplay.
        rec_gm = _read_byte(sock, f, 0x100, 'rec')
        emu_gm = _read_byte(sock, f, 0x100, 'emu')
        print(f'[entry] rec_GM=${rec_gm:02x} emu_GM=${emu_gm:02x}')

        # Sample Mario state every step for the next 200.
        print('\nstep | rX     eX     diff | rY     eY     diff | rS  eS  | r$15  e$15')
        prev_diff_y = None
        for fi in range(1, 201):
            cmd(sock, f, 'step 1')
            rx = _read_word(sock, f, 0xD1, 'rec')
            ex = _read_word(sock, f, 0xD1, 'emu')
            ry = _read_word(sock, f, 0xD3, 'rec')
            ey = _read_word(sock, f, 0xD3, 'emu')
            rs = _read_byte(sock, f, 0x7D, 'rec')
            es = _read_byte(sock, f, 0x7D, 'emu')
            r15 = _read_byte(sock, f, 0x15, 'rec')
            e15 = _read_byte(sock, f, 0x15, 'emu')
            diff_x = (rx - ex) & 0xFFFF
            diff_y = (ry - ey) & 0xFFFF
            mark = ''
            if rx != ex: mark += 'X'
            if ry != ey: mark += 'Y'
            if r15 != e15: mark += 'I'
            print(f'{fi:4} | ${rx:04x} ${ex:04x} ${diff_x:04x} | '
                  f'${ry:04x} ${ey:04x} ${diff_y:04x} | '
                  f'${rs:02x} ${es:02x} | ${r15:02x}  ${e15:02x}  {mark}')
    finally:
        try: sock.close()
        except Exception: pass
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill()
        _kill()


if __name__ == '__main__':
    main()
