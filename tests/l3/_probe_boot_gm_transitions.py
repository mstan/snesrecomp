"""Track GameMode ($0100) transitions on both sides from boot to
GM=07 (title screen). Recomp takes 207 frames, emu takes 204.
Find which GM stages account for the 3-frame skew.

Strategy: step both sides 1 frame at a time, sample $0100, log every
GM change with the boot-frame number on each side."""
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
    if side == 'rec':
        r = cmd(sock, f, f'dump_ram 0x{addr:x} 1')
    else:
        r = cmd(sock, f, f'emu_read_wram 0x{addr:x} 1')
    h = r.get('hex', '').replace(' ', '')
    return int(h, 16) if h else -1


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

        # Step each side independently and log GM changes.
        rec_log = []
        prev = -1
        for fi in range(1, 250):
            cmd(sock, f, 'step 1')
            gm = _read_byte(sock, f, 0x0100, 'rec')
            if gm != prev:
                rec_log.append((fi, gm))
                prev = gm
            if gm == 0x07: break

        emu_log = []
        prev = -1
        for fi in range(1, 250):
            cmd(sock, f, 'emu_step 1')
            gm = _read_byte(sock, f, 0x0100, 'emu')
            if gm != prev:
                emu_log.append((fi, gm))
                prev = gm
            if gm == 0x07: break

        print('=== RECOMP boot GM transitions ===')
        for fi, gm in rec_log:
            print(f'  frame {fi:4d}: GM=${gm:02x}')
        print(f'  total to GM=07: {rec_log[-1][0]} frames')

        print('\n=== EMU boot GM transitions ===')
        for fi, gm in emu_log:
            print(f'  frame {fi:4d}: GM=${gm:02x}')
        print(f'  total to GM=07: {emu_log[-1][0]} frames')

        # Per-stage delta. Pair by GM index where both sides hit
        # the same GM.
        rec_first = {gm: fi for fi, gm in rec_log}
        emu_first = {gm: fi for fi, gm in emu_log}
        all_gms = sorted(set(rec_first.keys()) | set(emu_first.keys()))
        print('\n=== per-GM first-entry skew (rec - emu) ===')
        for gm in all_gms:
            rf = rec_first.get(gm, -1)
            ef = emu_first.get(gm, -1)
            delta = rf - ef if (rf > 0 and ef > 0) else 'N/A'
            print(f'  GM=${gm:02x}: rec=f{rf:3d}, emu=f{ef:3d}, delta={delta}')

        # Per-stage time SPENT (next entry minus this entry).
        print('\n=== per-GM stage duration (next entry - this entry) ===')
        for log, name in [(rec_log, 'REC'), (emu_log, 'EMU')]:
            print(f'  {name}:')
            for i in range(len(log) - 1):
                fi, gm = log[i]
                next_fi, _ = log[i + 1]
                print(f'    GM=${gm:02x}: {next_fi - fi} frames')
    finally:
        try: sock.close()
        except Exception: pass
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill()
        _kill()


if __name__ == '__main__':
    main()
