"""Issue B: trace writers to Map16 buffer at a sample diverging
address ($7E:CE5C — uniformly $3F on rec, varied on emu).

Identify the routine that writes Map16 differently on each side.
That routine is Issue B's actual root cause.
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

        # Sample diverging Map16 addresses to investigate.
        for addr in (0xC800, 0xCB1D, 0xCE5C, 0xCE60, 0xCE65, 0xCE7E):
            print(f'\n=== ${addr:04x} ===')
            r = cmd(sock, f, f'wram_writes_at {addr:x} 0 999999 4096')
            rec_writes = r.get('matches', [])
            print(f'  rec: {len(rec_writes)} writes')
            # Top writers by func.
            funcs = Counter(e["func"] for e in rec_writes)
            for fn, n in funcs.most_common(5):
                print(f'    {n:5}  {fn}')
            for e in rec_writes[:8]:
                print(f'    f={e["f"]:5} val={e["val"]:>6} '
                      f'func={e["func"][:32]:32}')

            er = cmd(sock, f, f'emu_wram_writes_at {addr:x} 0 999999 4096')
            emu_writes = er.get('matches', [])
            print(f'  emu: {len(emu_writes)} writes')
            pcs = Counter(e["pc"] for e in emu_writes)
            for pc, n in pcs.most_common(5):
                print(f'    {n:5}  pc={pc}')
            for e in emu_writes[:8]:
                print(f'    f={e["f"]:5} pc={e["pc"]} '
                      f'{e["before"]}->{e["after"]}')
    finally:
        try: sock.close()
        except Exception: pass
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill()
        _kill()


if __name__ == '__main__':
    main()
