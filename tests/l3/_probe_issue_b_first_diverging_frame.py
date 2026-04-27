"""Issue B: find the FIRST frame in [TI=$04 .. TI=$06] window
where $8A or $98 first diverges between rec and emu.

Both sides ran 48 frames between TI=$04 (rec_frame=216,
emu_frame=44485) and TI=$06 (rec_frame=264, emu_frame=44533).
Linear mapping: rec_frame K -> emu_frame K + 44269.

For each frame K in [216, 264], compare $8A, $98, $9A on both
sides. The first K where any byte diverges is the seed frame —
the very first moment of demo desync.
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
    h = cmd(sock, f, c).get('hex', '').replace(' ', '')
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

        for _ in range(3000):
            cmd(sock, f, 'step 1')
            if _read_byte(sock, f, 0x100, 'rec') == 0x07: break
        for _ in range(800):
            cmd(sock, f, 'step 1')

        # Find rec/emu frames for TI=$04 and TI=$06.
        rec_writes = cmd(sock, f, 'wram_writes_at 1df4 0 999999 4096').get('matches', [])
        rec_byval = {}
        for e in rec_writes:
            v = int(e['val'], 16) & 0xFF
            if v not in rec_byval: rec_byval[v] = int(e['f'])

        h = cmd(sock, f, 'emu_history')
        oldest = h.get('oldest', -1); newest = h.get('newest', -1)
        emu_byval = {}
        prev_v = None
        for fi in range(oldest, newest + 1):
            r = cmd(sock, f, f'emu_wram_at_frame {fi} 1df4')
            if not r.get('ok'): continue
            v = int(r['val'], 16) & 0xFF
            if v != prev_v:
                if v not in emu_byval: emu_byval[v] = fi
                prev_v = v

        rec_04 = rec_byval[0x04]; emu_04 = emu_byval[0x04]
        rec_06 = rec_byval[0x06]; emu_06 = emu_byval[0x06]
        offset = emu_04 - rec_04   # emu_frame = rec_frame + offset
        print(f'rec TI=$04 at {rec_04}; emu TI=$04 at {emu_04}; offset={offset}')
        print(f'rec TI=$06 at {rec_06}; emu TI=$06 at {emu_06}')

        # Walk every frame in [rec_04, rec_06], read $8A, $98, $9A
        # on both sides, find the first divergence.
        ADDRS = [(0x8A, 'PlayerBlockCol'), (0x98, 'TouchBlockYPos_lo'),
                 (0x9A, 'TouchBlockXPos_lo')]
        first_div = {addr: None for addr, _ in ADDRS}

        print(f'\nframe | rec(8A 98 9A) | emu(8A 98 9A) | div')
        for rec_f in range(rec_04, rec_06 + 1):
            emu_f = rec_f + offset
            row_rec = []
            row_emu = []
            div = ''
            for addr, label in ADDRS:
                rr = cmd(sock, f, f'dump_frame_wram {rec_f} {addr:x} 1')
                rh = rr.get('hex', '').replace(' ', '')
                r = int(rh, 16) if rh else -1
                er = cmd(sock, f, f'emu_wram_at_frame {emu_f} {addr:x}')
                e = int(er.get('val', '0xff'), 16) if er.get('ok') else -1
                row_rec.append(f'{r:02x}')
                row_emu.append(f'{e:02x}')
                if r != e:
                    div += f' ${addr:02x}'
                    if first_div[addr] is None:
                        first_div[addr] = rec_f
            if div or rec_f in (rec_04, rec_04+1, rec_04+2, rec_06):
                print(f'  {rec_f:3} | {" ".join(row_rec):8} | '
                      f'{" ".join(row_emu):8} |{div}')

        print('\nfirst divergence per address:')
        for addr, label in ADDRS:
            print(f'  ${addr:02x} {label}: rec_frame {first_div[addr]}')
    finally:
        try: sock.close()
        except Exception: pass
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill()
        _kill()


if __name__ == '__main__':
    main()
