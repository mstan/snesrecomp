"""Issue B: trace writes to $6B/$6C (Map16LowPtr) on both sides
during the level-load window.

The diagonal-ledge function body matches ROM byte-for-byte. The
bug must be in one of the pointer-advance helpers it calls. The
helpers update $6B (Map16LowPtr lo) and $6C (Map16LowPtr hi).

If rec and emu's $6B/$6C update streams diverge at any point
during the level-load window, the function whose write produced
the divergence is the bug source.
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

        # Pull Map16LowPtr ($6B) write stream from both sides.
        # Frame 94 is the level-load frame on rec.
        for addr, label in [(0x6B, 'Map16LowPtr_lo'),
                            (0x6C, 'Map16LowPtr_hi')]:
            print(f'\n=== ${addr:02x} ({label}) ===')
            r = cmd(sock, f, f'wram_writes_at {addr:x} 90 100 4096')
            rec_writes = r.get('matches', [])
            print(f'  rec: {len(rec_writes)} writes (frames 90-100)')
            funcs = Counter(e["func"] for e in rec_writes)
            for fn, n in funcs.most_common(8):
                print(f'    {n:5}  {fn}')

            er = cmd(sock, f, f'emu_wram_writes_at {addr:x} 0 999999 4096')
            emu_writes = er.get('matches', [])
            print(f'  emu: {len(emu_writes)} writes (all time)')
            pcs = Counter(e["pc"] for e in emu_writes)
            for pc, n in pcs.most_common(8):
                print(f'    {n:5}  pc={pc}')

        # Now do a SEQUENCE comparison: dump 30 consecutive writes
        # from each side, see at what step the value sequences
        # diverge.
        print('\n=== rec $6B sequence (chronological, first 30 writes after frame 94) ===')
        r = cmd(sock, f, 'wram_writes_at 6b 0 999999 4096')
        rec = [e for e in r.get('matches', []) if e['f'] >= 94][:30]
        for e in rec:
            print(f'  f={e["f"]:4} val={e["val"]:>6} func={e["func"][:42]}')
    finally:
        try: sock.close()
        except Exception: pass
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill()
        _kill()


if __name__ == '__main__':
    main()
