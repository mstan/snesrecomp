"""Issue B: trace Map16TileNumber ($1693) writes.

Every F44D call writes the tile id to $1693. The values across
the sink window tell us what F44D actually returned for each
interaction-point probe. Compare:

  - rec stream of $1693 values during sink frames vs
  - emu stream of $1693 values when emu was at the same X.

If rec stream contains tile $25 at the foot-probe call (which
should make collision treat it as solid), the bug is downstream.
If rec stream doesn't contain $25 where it should (e.g., always
$00), F44D reads wrong addresses on the recomp side.
"""
from __future__ import annotations
import json, pathlib, socket, subprocess, sys, time
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

        # Step into gameplay and let demo run a bit so Mario passes
        # through sink-X positions.
        for _ in range(3000):
            cmd(sock, f, 'step 1')
            if _read_byte(sock, f, 0x100, 'rec') == 0x07: break
        for _ in range(800):
            cmd(sock, f, 'step 1')

        rec_x = _read_word(sock, f, 0xD1, 'rec')
        rec_y = _read_word(sock, f, 0xD3, 'rec')
        rec_frame = cmd(sock, f, 'frame').get('frame', '?')
        print(f'rec: frame={rec_frame} X=${rec_x:04x} Y=${rec_y:04x}')

        # Pull the entire $1693 trace.
        r = cmd(sock, f, 'wram_writes_at 1693 0 999999 4096')
        rec_writes = r.get('matches', [])
        print(f'\n[rec] $1693 (Map16TileNumber) writes: {len(rec_writes)}')
        # Distribution of values written.
        rec_vals = Counter(e["val"] for e in rec_writes)
        print(f'  value distribution (top 20):')
        for v, n in rec_vals.most_common(20):
            print(f'    {n:5}  {v}')

        # Emu side.
        r = cmd(sock, f, 'emu_wram_writes_at 1693 0 999999 4096')
        emu_writes = r.get('matches', [])
        print(f'\n[emu] $1693 writes: {len(emu_writes)}')
        emu_vals = Counter(e["after"] for e in emu_writes)
        print(f'  value distribution (top 20):')
        for v, n in emu_vals.most_common(20):
            print(f'    {n:5}  {v}')

        # Side-by-side: which tile ids does each side encounter?
        rec_tiles = set(int(e["val"], 16) & 0xFF for e in rec_writes)
        emu_tiles = set(int(e["after"], 16) & 0xFF for e in emu_writes)
        only_rec = sorted(rec_tiles - emu_tiles)
        only_emu = sorted(emu_tiles - rec_tiles)
        both = sorted(rec_tiles & emu_tiles)
        print(f'\ntile ids seen by both: {[hex(t) for t in both]}')
        print(f'only rec sees: {[hex(t) for t in only_rec]}')
        print(f'only emu sees: {[hex(t) for t in only_emu]}')

        if 0x25 in rec_tiles:
            print('\n=> rec DOES write $25 to Map16TileNumber. F44D returns $25 sometimes.')
            print('   Bug is downstream of F44D — in how RunPlayerBlockCode_EB77')
            print('   consumes the return value, OR foot-probe X is different.')
        else:
            print('\n=> rec NEVER writes $25 to Map16TileNumber. F44D never returns $25.')
            print('   Either F44D reads wrong addresses, or Mario never probes the')
            print('   solid-tile rows on rec (foot-probe Y is different).')

        if 0x25 in emu_tiles:
            print('=> emu DOES write $25 to Map16TileNumber.')
        else:
            print('=> emu NEVER writes $25 to Map16TileNumber. (Suspicious; would need')
            print('   to verify with longer recording or different X positions.)')
    finally:
        try: sock.close()
        except Exception: pass
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill()
        _kill()


if __name__ == '__main__':
    main()
