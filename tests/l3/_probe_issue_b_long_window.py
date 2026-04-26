"""Issue B phase 4: 800-frame post-GM07 sample at every 5 frames.
Looking for: (a) when do recomp/emu Mario X re-converge after demo
desync, (b) what frame ranges have Mario near Y=$0150 (ground), and
(c) whether recomp ever shows the "1-tile-under" Y=$0160 / $015X
state vs emu's Y=$0150."""
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


def _read_word(sock, f, addr, side):
    lo = _read_byte(sock, f, addr, side)
    hi = _read_byte(sock, f, addr + 1, side)
    return (hi << 8) | lo


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

        # GM=07 sync
        for _ in range(3000):
            cmd(sock, f, 'step 1')
            if _read_byte(sock, f, 0x100, 'rec') == 0x07: break
        for _ in range(3000):
            cmd(sock, f, 'emu_step 1')
            if _read_byte(sock, f, 0x100, 'emu') == 0x07: break

        print(' fr  |  rX  |  eX  |  rY  |  eY  | rS | eS | rP/eP | r15 e15')
        first_x_match_after_sync = None
        for big in range(0, 1000, 5):
            for _ in range(5):
                cmd(sock, f, 'step 1'); cmd(sock, f, 'emu_step 1')
            fi = big + 5
            rx = _read_word(sock, f, 0xD1, 'rec')
            ex = _read_word(sock, f, 0xD1, 'emu')
            ry = _read_word(sock, f, 0xD3, 'rec')
            ey = _read_word(sock, f, 0xD3, 'emu')
            rs = _read_byte(sock, f, 0x7D, 'rec')
            es = _read_byte(sock, f, 0x7D, 'emu')
            rp = _read_byte(sock, f, 0x13E0, 'rec')
            ep = _read_byte(sock, f, 0x13E0, 'emu')
            r15 = _read_byte(sock, f, 0x15, 'rec')
            e15 = _read_byte(sock, f, 0x15, 'emu')
            m = ''
            if rx != ex: m += 'X'
            if ry != ey: m += 'Y'
            if rs != es: m += 'S'
            if r15 != e15: m += 'I'
            print(f'+{fi:4d} | ${rx:04x} | ${ex:04x} | ${ry:04x} | ${ey:04x} | '
                  f'${rs:02x} | ${es:02x} | ${rp:02x}/${ep:02x} | '
                  f'${r15:02x}/${e15:02x}  {m}')
            if fi >= 50 and rx == ex and first_x_match_after_sync is None:
                first_x_match_after_sync = fi
        print(f'\n[summary] first X re-convergence after sync: '
              f'{first_x_match_after_sync}')
    finally:
        try: sock.close()
        except Exception: pass
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill()
        _kill()


if __name__ == '__main__':
    main()
