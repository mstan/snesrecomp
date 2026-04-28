"""Issue B: trace $D3/$D4 (Mario Y) writers at the X=$01C7
waypoint on both sides. The first divergent write identifies
the responsible code path.

Methodology (Bug #8 chain):
  1. Step rec to Mario X = TARGET_X (auto-locksteps emu).
  2. Find emu's history frame where emu was at TARGET_X.
  3. Query both sides' always-on WRAM trace for writers to
     $D3 / $D4 around their respective waypoint frames.
  4. List both write streams; the first PC/func that DIFFERS
     in value at the same logical position is the seed.
"""
from __future__ import annotations
import json, pathlib, socket, subprocess, time

REPO = pathlib.Path(__file__).parent.parent.parent.parent
EXE = REPO / 'build' / 'bin-x64-Oracle' / 'smw.exe'
PORT = 4377

TARGET_X = 0x01C7


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

        # Step rec until X=$01C7.
        rec_step_at_target = None
        for fi in range(1, 1500):
            cmd(sock, f, 'step 1')
            if _read_word(sock, f, 0xD1, 'rec') == TARGET_X:
                rec_step_at_target = fi; break
        if rec_step_at_target is None:
            print('rec did not hit target X'); return
        print(f'[rec] reached X=${TARGET_X:04x} at gameplay step {rec_step_at_target}')

        # Sample state at waypoint.
        rec_y = _read_word(sock, f, 0xD3, 'rec')
        rec_frame = cmd(sock, f, 'frame').get('frame', '?')
        print(f'  rec_frame={rec_frame} rec_Y=${rec_y:04x}')

        # Find emu's matching frame.
        r = cmd(sock, f, f'emu_history_find_word d1 {TARGET_X:x}')
        emu_frame = r.get('frame', -1)
        if emu_frame < 0:
            print('emu has no matching frame in history'); return
        emu_y_lo = int(cmd(sock, f, f'emu_wram_at_frame {emu_frame} d3').get('val', '0x0'), 16)
        emu_y_hi = int(cmd(sock, f, f'emu_wram_at_frame {emu_frame} d4').get('val', '0x0'), 16)
        emu_y = (emu_y_hi << 8) | emu_y_lo
        print(f'[emu] X=${TARGET_X:04x} at history frame {emu_frame} '
              f'emu_Y=${emu_y:04x}')

        # Query rec's $D3/$D4 writers in the recent past
        # (last ~50 rec frames).
        print(f'\n=== REC writers to $D3/$D4 (last ~50 frames before waypoint) ===')
        for addr in (0xD3, 0xD4):
            r = cmd(sock, f, f'wram_writes_at {addr:x} 0 999999 30')
            writes = r.get('matches', [])
            recent = writes[-15:] if len(writes) > 15 else writes
            print(f'  ${addr:02x}: {len(writes)} writes total, showing last {len(recent)}:')
            for e in recent:
                print(f'    f={e["f"]:5} val={e["val"]:>6} '
                      f'func={e["func"][:30]:30} parent={e["parent"][:25]}')

        # Query emu's $D3/$D4 writers around emu's waypoint frame.
        # Probe with no window first to see what frames the writes
        # actually land at.
        print(f'\n=== EMU writers to $D3/$D4 (last 30 in unbounded window) ===')
        for addr in (0xD3, 0xD4):
            r = cmd(sock, f, f'emu_wram_writes_at {addr:x} 0 999999 30')
            writes = r.get('matches', [])
            print(f'  ${addr:02x}: {len(writes)} writes total, showing last 15:')
            for e in writes[-15:]:
                print(f'    f={e["f"]:5} pc={e["pc"]} '
                      f'{e["before"]}->{e["after"]} bank={e["bank_src"]}')
    finally:
        try: sock.close()
        except Exception: pass
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill()
        _kill()


if __name__ == '__main__':
    main()
