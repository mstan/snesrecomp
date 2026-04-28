"""Issue B with waypoint-pinned sync. Step both sides together
in lockstep; whenever rec_X happens to equal emu_X, sample full
Mario state (Y, speed, pose, ground flag, $7B-$80) and compare.
At the matched-X frames, we ARE at the same world position, so
the comparison is meaningful.

Looking for: frames where rec_X == emu_X but rec_Y != emu_Y
(Mario at same X, different Y — Issue B's signature)."""
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
    h = cmd(sock, f, c).get('hex', '').replace(' ', '')
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

        for _ in range(3000):
            cmd(sock, f, 'step 1')
            if _read_byte(sock, f, 0x100, 'rec') == 0x07: break

        # Step both into gameplay (past title screen).
        for _ in range(50):
            cmd(sock, f, 'step 1')

        # Sample 1000 steps; record matches where rec_X == emu_X.
        matches = []
        any_diff_y = []
        for fi in range(1, 1001):
            cmd(sock, f, 'step 1')
            rx = _read_word(sock, f, 0xD1, 'rec')
            ex = _read_word(sock, f, 0xD1, 'emu')
            if rx == ex:
                ry = _read_word(sock, f, 0xD3, 'rec')
                ey = _read_word(sock, f, 0xD3, 'emu')
                rs = _read_byte(sock, f, 0x7D, 'rec')
                es = _read_byte(sock, f, 0x7D, 'emu')
                rg = _read_byte(sock, f, 0x77, 'rec')  # PlayerBlockedDir
                eg = _read_byte(sock, f, 0x77, 'emu')
                rp = _read_byte(sock, f, 0x13E0, 'rec')
                ep = _read_byte(sock, f, 0x13E0, 'emu')
                matches.append((fi, rx, ry, ey, rs, es, rg, eg, rp, ep))
                if ry != ey:
                    any_diff_y.append((fi, rx, ry, ey, rs, es, rg, eg, rp, ep))

        print(f'\nTotal X-match frames in 1000 steps: {len(matches)}')
        print(f'Of those, frames with Y diff: {len(any_diff_y)}')
        if matches:
            print('\nFirst 30 X-match frames:')
            print('step | X     | rY     eY     | rS  eS  | r$77 e$77 | rPose ePose')
            for m in matches[:30]:
                fi, rx, ry, ey, rs, es, rg, eg, rp, ep = m
                mark = ' Y!' if ry != ey else ''
                print(f'{fi:4} | ${rx:04x} | ${ry:04x} ${ey:04x} | '
                      f'${rs:02x} ${es:02x} | ${rg:02x}  ${eg:02x}  | '
                      f' ${rp:02x}   ${ep:02x}  {mark}')
        if any_diff_y:
            print('\nFRAMES WITH SAME X, DIFFERENT Y (Issue B candidates):')
            for m in any_diff_y[:30]:
                fi, rx, ry, ey, rs, es, rg, eg, rp, ep = m
                print(f'  step {fi:4}: X=${rx:04x} rY=${ry:04x} eY=${ey:04x} '
                      f'(diff={(ry-ey)&0xFFFF:+x}) rS=${rs:02x} eS=${es:02x}')
    finally:
        try: sock.close()
        except Exception: pass
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill()
        _kill()


if __name__ == '__main__':
    main()
