"""Issue B cross-check: at the sink position, does snes9x ALSO
place Mario at Y=$0160?

Uses the snes9x per-frame WRAM history ring. Find the most recent
emu frame where Mario X was at one of the observed-sink X values
(e.g. $01C7 or $05DD); read Mario Y from that history frame.

If emu Y == rec Y at the same X, Mario is correctly geometrically
positioned and Issue B is not a codegen bug. If they differ, real
divergence.

Run against the Oracle build (which has both sides).
"""
from __future__ import annotations
import json, pathlib, socket, subprocess, sys, time

REPO = pathlib.Path(__file__).parent.parent.parent.parent
EXE = REPO / 'build' / 'bin-x64-Oracle' / 'smw.exe'
PORT = 4377

# X positions where the watchdog observed Mario at Y=$0160.
SINK_X_CANDIDATES = [0x01C7, 0x0333, 0x0395, 0x05A6, 0x05CD, 0x05DD,
                     0x0612, 0x06FB]


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

        # Step both into gameplay. Long enough that emu has visited
        # all the candidate X positions.
        for _ in range(3000):
            cmd(sock, f, 'step 1')
            if _read_byte(sock, f, 0x100, 'rec') == 0x07: break
        for _ in range(800):
            cmd(sock, f, 'step 1')

        history = cmd(sock, f, 'emu_history')
        print(f'emu history: {history}')
        print()

        # For each candidate X, query emu's history for the frame
        # where its $D1 was that value, then read Mario Y from that
        # history frame.
        print('cross-check Mario Y at sink-X positions (rec vs emu):')
        print('  X       rec_Y_now  emu_Y_at_history_frame   delta   verdict')
        for x in SINK_X_CANDIDATES:
            r = cmd(sock, f, f'emu_history_find_word d1 {x:x}')
            emu_frame = r.get('frame', -1)
            if emu_frame < 0:
                print(f'  ${x:04x}  (no emu history match)')
                continue
            emu_y_lo = int(cmd(sock, f, f'emu_wram_at_frame {emu_frame} d3').get('val', '0x0'), 16)
            emu_y_hi = int(cmd(sock, f, f'emu_wram_at_frame {emu_frame} d4').get('val', '0x0'), 16)
            emu_y = (emu_y_hi << 8) | emu_y_lo

            # Sample rec's Mario Y if rec is at this X right now.
            rec_x = _read_word(sock, f, 0xD1, 'rec')
            if rec_x == x:
                rec_y = _read_word(sock, f, 0xD3, 'rec')
            else:
                rec_y = None
            verdict = ''
            if rec_y is not None:
                if rec_y == emu_y:
                    verdict = 'MATCH (no codegen bug)'
                else:
                    verdict = f'DIFFER (delta={rec_y-emu_y:+d})'
            else:
                verdict = '(rec not at this X right now)'
            rec_y_str = f'${rec_y:04x}' if rec_y is not None else '   --'
            print(f'  ${x:04x}    {rec_y_str}     ${emu_y:04x} (frame {emu_frame})    '
                  f'{verdict}')

        # Bonus: just dump the most recent emu frame's Y at the X
        # rec is currently at. Most relevant comparison.
        rec_x_now = _read_word(sock, f, 0xD1, 'rec')
        rec_y_now = _read_word(sock, f, 0xD3, 'rec')
        print(f'\nrec right now: X=${rec_x_now:04x} Y=${rec_y_now:04x}')
        r = cmd(sock, f, f'emu_history_find_word d1 {rec_x_now:x}')
        ef = r.get('frame', -1)
        if ef >= 0:
            ely = int(cmd(sock, f, f'emu_wram_at_frame {ef} d3').get('val', '0x0'), 16)
            ehy = int(cmd(sock, f, f'emu_wram_at_frame {ef} d4').get('val', '0x0'), 16)
            ey = (ehy << 8) | ely
            print(f'  emu most-recent Y at this X: ${ey:04x} (frame {ef})')
            print(f'  delta: {rec_y_now - ey:+d}')
    finally:
        try: sock.close()
        except Exception: pass
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill()
        _kill()


if __name__ == '__main__':
    main()
