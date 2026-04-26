"""Issue B with snes9x history-ring waypoint sync.

Strategy:
  1. Step both sides past GM=07 + warmup into gameplay.
  2. Step rec-side until rec_X (= g_ram[$D1]) hits a target value
     (the X position where Issue B's "Mario sinks" was observed,
     X=$01C7 from earlier probes).
  3. Query snes9x's history ring: which emu-frame was emu's $D1
     last == $01C7? At THAT emu-frame, what was emu's $D3 (Mario Y)?
  4. Compare: rec's Mario Y at rec's "Mario at X=$01C7" moment
     vs emu's Mario Y at emu's "Mario at X=$01C7" moment.

If rec_Y at X=$01C7 is $0160 (1 tile under) and emu_Y at X=$01C7
is $0150 (ground), Issue B's signature is reproduced cleanly via
position-pinned sync rather than step-pinned.
"""
from __future__ import annotations
import json, pathlib, socket, subprocess, time

REPO = pathlib.Path(__file__).parent.parent.parent.parent
EXE = REPO / 'build' / 'bin-x64-Oracle' / 'smw.exe'
PORT = 4377

TARGET_X = 0x01C7  # observed sink X from prior probes


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

        # GM=07 sync (auto-locksteps emu).
        for _ in range(3000):
            cmd(sock, f, 'step 1')
            if _read_byte(sock, f, 0x100, 'rec') == 0x07: break

        target_lo = TARGET_X & 0xFF
        target_hi = (TARGET_X >> 8) & 0xFF
        print(f'[GM=07 sync done] target Mario X = ${TARGET_X:04x} '
              f'(lo=${target_lo:02x}, hi=${target_hi:02x})')

        # Step until rec_X reaches target.
        rec_steps_in_gameplay = 0
        for fi in range(1, 1500):
            cmd(sock, f, 'step 1'); rec_steps_in_gameplay += 1
            rx = _read_word(sock, f, 0xD1, 'rec')
            if rx == TARGET_X:
                print(f'  rec hit X=${TARGET_X:04x} at gameplay step {fi}')
                break
        else:
            rx = _read_word(sock, f, 0xD1, 'rec')
            print(f'  rec did NOT hit target X within 1500 steps; final '
                  f'rec_X=${rx:04x}')
            return

        # Sample rec's Mario state at the moment rec_X = target.
        rec_y = _read_word(sock, f, 0xD3, 'rec')
        rec_yspd = _read_byte(sock, f, 0x7D, 'rec')
        rec_blockdir = _read_byte(sock, f, 0x77, 'rec')
        rec_pose = _read_byte(sock, f, 0x13E0, 'rec')
        print(f'\n[rec at X=${TARGET_X:04x}]')
        print(f'  Y=${rec_y:04x}  Yspd=${rec_yspd:02x}  '
              f'BlockedDir=${rec_blockdir:02x}  Pose=${rec_pose:02x}')

        # Query history ring for emu's most-recent frame where
        # emu's 16-bit X (at $D1:$D2) == TARGET_X.
        r = cmd(sock, f, f'emu_history_find_word d1 {TARGET_X:x}')
        emu_frame = r.get('frame', -1)
        print(f'\n[emu history search] last frame where emu_X=${TARGET_X:04x}: '
              f'frame {emu_frame}')
        if emu_frame < 0:
            print('  emu has not visited this X position in its history.')
            print('  Try a different target or longer history retention.')
            return

        # At emu_frame, sample emu's full Mario state from history.
        for label, addr in [('Y_lo', 0xD3), ('Y_hi', 0xD4),
                            ('Yspd', 0x7D), ('BlockedDir', 0x77),
                            ('Pose', 0x13E0)]:
            r = cmd(sock, f, f'emu_wram_at_frame {emu_frame} {addr:x}')
            v = int(r.get('val', '0x0'), 16) if r.get('ok') else -1
            print(f'  emu_{label} (${addr:04x}) at frame {emu_frame}: ${v:02x}')

        emu_y_lo = int(cmd(sock, f, f'emu_wram_at_frame {emu_frame} d3').get('val', '0x0'), 16)
        emu_y_hi = int(cmd(sock, f, f'emu_wram_at_frame {emu_frame} d4').get('val', '0x0'), 16)
        emu_y = (emu_y_hi << 8) | emu_y_lo
        print(f'\n=== ISSUE B SIGNATURE CHECK ===')
        print(f'  rec Y at X=${TARGET_X:04x}: ${rec_y:04x}')
        print(f'  emu Y at X=${TARGET_X:04x}: ${emu_y:04x}')
        delta = (rec_y - emu_y) & 0xFFFF
        print(f'  Y delta (rec - emu): ${delta:04x} ({(rec_y - emu_y):+d} pixels)')
        if rec_y != emu_y:
            print(f'  *** Mario at same X, different Y — Issue B reproduced ***')
        else:
            print(f'  No Y divergence at this X. Try other waypoints.')
    finally:
        try: sock.close()
        except Exception: pass
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill()
        _kill()


if __name__ == '__main__':
    main()
