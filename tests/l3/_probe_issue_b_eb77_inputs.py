"""Issue B: check the inputs to RunPlayerBlockCode_EB77's X-set
chain at TI=$04 and TI=$06.

EB77 sets the F44D interaction-point index (X register) based on:
  $0019 Powerup
  $0073 PlayerIsDucking
  $187A PlayerRidingYoshi

Plus position bytes ($94/$95 PlayerXPosNext) used in column math.

If any of these differ at TI=$04 (the last clean sync), the seed
is further upstream. If they all match at TI=$04 but EB77 still
produces different X by TI=$06, the codegen for EB77 has a bug.
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

        def check_at_ti(ti):
            rec_f = rec_byval.get(ti); emu_f = emu_byval.get(ti)
            if rec_f is None or emu_f is None:
                print(f'TI=${ti:02x} not common'); return
            print(f'\n=== TI=${ti:02x} (rec_frame={rec_f}, emu_frame={emu_f}) ===')
            fields = [
                (0x0019, 'Powerup'),
                (0x0073, 'PlayerIsDucking'),
                (0x187A, 'PlayerRidingYoshi'),
                (0x0094, 'PlayerXPosNext_lo'),
                (0x0095, 'PlayerXPosNext_hi'),
                (0x0096, 'PlayerYPosNext_lo'),
                (0x0097, 'PlayerYPosNext_hi'),
                (0x0090, 'PlayerYPosInBlock'),
                (0x0091, 'PlayerBlockMoveY'),
                (0x0092, 'PlayerXPosInBlock'),
                (0x0093, 'PlayerBlockXSide'),
                (0x008A, 'PlayerBlockCol'),
                (0x008D, 'TempPlayerGround'),
                (0x008F, 'TempPlayerAir'),
                (0x0098, 'TouchBlockYPos_lo'),
                (0x0099, 'TouchBlockYPos_hi'),
                (0x009A, 'TouchBlockXPos_lo'),
                (0x009B, 'TouchBlockXPos_hi'),
                (0x007D, 'PlayerYSpeed'),
                (0x007B, 'PlayerXSpeed'),
                (0x0072, 'PlayerInAir'),
                (0x0077, 'PlayerBlockedDir'),
            ]
            diffs = 0
            for addr, label in fields:
                # rec via dump_frame_wram
                rr = cmd(sock, f, f'dump_frame_wram {rec_f} {addr:x} 1')
                rh = rr.get('hex', '').replace(' ', '')
                r = int(rh, 16) if rh else -1
                # emu via emu_wram_at_frame (only $0..$1FFF window)
                if addr < 0x2000:
                    er = cmd(sock, f, f'emu_wram_at_frame {emu_f} {addr:x}')
                    e = int(er.get('val', '0xff'), 16) if er.get('ok') else -1
                else:
                    e = -1  # outside snapshot window
                mark = ''
                if r != e and e != -1:
                    diffs += 1; mark = '  <- DIFF'
                e_str = f'${e:02x}' if e != -1 else 'N/A'
                print(f'  ${addr:04x} {label:22} rec=${r:02x}  emu={e_str}{mark}')
            print(f'  total diffs: {diffs}')

        check_at_ti(0x04)
        check_at_ti(0x06)
    finally:
        try: sock.close()
        except Exception: pass
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill()
        _kill()


if __name__ == '__main__':
    main()
