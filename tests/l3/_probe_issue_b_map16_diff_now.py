"""Issue B: diff Map16 buffer ($7E:C800-$E800) on rec vs emu RIGHT NOW.

If the level data Map16 buffers match between sides currently (and
the attract demo doesn't modify Map16 mid-play), they certainly
matched at frame 216 too. In that case F44D reads the same tiles
on both sides — so the divergence must be in F44D's own recomp
body (codegen bug).

If Map16 differs even now, the bug is in level-data loading or
in some routine that writes Map16 differently on each side.
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


def _read_range(sock, f, side, addr, length):
    out = bytearray()
    while length > 0:
        n = min(1024, length)
        c = (f'dump_ram 0x{addr:x} {n}' if side == 'rec'
             else f'emu_read_wram 0x{addr:x} {n}')
        r = cmd(sock, f, c)
        out.extend(bytes.fromhex(r.get('hex', '').replace(' ', '')))
        addr += n; length -= n
    return bytes(out)


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

        # Step into gameplay so Map16 is loaded.
        for _ in range(3000):
            cmd(sock, f, 'step 1')
            r = cmd(sock, f, 'dump_ram 0x100 1')['hex'].replace(' ', '')
            if int(r, 16) == 0x07: break
        for _ in range(800):
            cmd(sock, f, 'step 1')

        print('reading Map16Lo $7E:C800-$E800 (8KB) from rec...')
        rec = _read_range(sock, f, 'rec', 0xC800, 0x2000)
        print('reading Map16Lo from emu...')
        emu = _read_range(sock, f, 'emu', 0xC800, 0x2000)

        diffs = []
        for i in range(len(rec)):
            if rec[i] != emu[i]:
                if rec[i] == 0 and emu[i] == 0x55:
                    continue   # init-policy noise
                diffs.append((0xC800 + i, rec[i], emu[i]))

        print(f'\nMap16 diffs (excluding init-policy): {len(diffs)}')
        for a, r, e in diffs[:40]:
            print(f'  $7E:{a:04x}: rec=${r:02x} emu=${e:02x}')
        if len(diffs) > 40:
            print(f'  +{len(diffs) - 40} more')

        if not diffs:
            print('\n=> Map16 buffers MATCH between sides.')
            print('   F44D reads the same tile data on both sides.')
            print('   The 1-frame divergence at rec_frame 217 must be in')
            print("   F44D's recomp codegen body itself, OR in CPU register")
            print('   state (X passed to F44D) somehow differing despite')
            print('   matching WRAM inputs.')
        else:
            print('\n=> Map16 buffers DIFFER. Level data loaded differently.')
            print('   Investigate level-data load chain (CODE_05D796 etc.).')
    finally:
        try: sock.close()
        except Exception: pass
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill()
        _kill()


if __name__ == '__main__':
    main()
