"""Issue B: identify which object-handler is called extra times
on rec, causing Map16 over-write.

Approach: query the always-on trace for every write to $7E:CE5C
during the level-load window, capture the full func+parent
attribution. The pattern of (func, parent) tells us which
object handler chain wrote each value. The handler whose write
overwrites the correct prior tile is the bug source.

Also: count total $7E:C800-$E800 writes per writer-function on
both sides. If rec has MORE invocations of any handler than
emu, that's the over-dispatch culprit.
"""
from __future__ import annotations
import json, pathlib, socket, subprocess, time
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

        # 1. Per-CE5C writes with full func+parent attribution.
        print('=== rec writes to $7E:CE5C (full attribution) ===')
        r = cmd(sock, f, 'wram_writes_at ce5c 0 999999 4096')
        for e in r.get('matches', []):
            print(f'  f={e["f"]:5} val={e["val"]:>6} '
                  f'func={e["func"][:36]:36} '
                  f'parent={e["parent"][:30]}')

        # 2. Sample several diverging Map16 cells, see which
        #    parent-funcs wrote the WRONG value on rec.
        print('\n=== rec writes to additional diverging Map16 cells ===')
        for cell in (0xCE60, 0xCE65, 0xCE73, 0xCE7E, 0xCB1D, 0xC800):
            r = cmd(sock, f, f'wram_writes_at {cell:x} 0 999999 4096')
            print(f'\n--- $7E:{cell:04x} ---')
            for e in r.get('matches', []):
                print(f'  f={e["f"]:5} val={e["val"]:>6} '
                      f'func={e["func"][:36]:36} '
                      f'parent={e["parent"][:30]}')

        # 3. Count per-parent calls to HandleHorizontalSubScreenCrossing
        #    across the whole Map16 range. Diff rec vs emu by parent.
        print('\n=== rec: per-parent call counts (HandleHorizontal across Map16) ===')
        # Walk a sample of Map16 addrs and accumulate.
        rec_parent_counts = Counter()
        for cell in range(0xC800, 0xE800, 0x40):
            r = cmd(sock, f, f'wram_writes_at {cell:x} 0 999999 32')
            for e in r.get('matches', []):
                if 'HandleHorizontal' in e.get('func', ''):
                    rec_parent_counts[e.get('parent', '')] += 1
        for par, n in rec_parent_counts.most_common(15):
            print(f'  {n:5}  {par}')
    finally:
        try: sock.close()
        except Exception: pass
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill()
        _kill()


if __name__ == '__main__':
    main()
