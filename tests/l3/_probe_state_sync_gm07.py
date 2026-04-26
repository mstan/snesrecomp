"""State-sync diff at GM=07 entry, skipping DP scratch ($00-$1F)
and stack page ($0100-$01FF). Bug #8 used this same shape and
found $72=$24 vs $00 as the seed.

Issue B (Mario sinks 1 tile near Yoshi block) is a downstream
visible bug. Per the established golden-oracle methodology, look
for upstream state divergence at the GM=07 entry sync point that
could be the seed driving Issue B.
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
            r = cmd(sock, f, 'dump_ram 0x100 1')['hex'].replace(' ', '')
            if int(r, 16) == 0x07: break
        for _ in range(3000):
            cmd(sock, f, 'emu_step 1')
            r = cmd(sock, f, 'emu_read_wram 0x100 1')['hex'].replace(' ', '')
            if int(r, 16) == 0x07: break

        # Scan ranges that exclude noise.
        # $20-$FF: real DP game state
        # $200-$1FFF: low WRAM + sprite tables
        # $2000-$7FFF: extended state
        ranges = [
            ('DP_state', 0x20, 0xFF),
            ('low_WRAM', 0x200, 0x1FFF),
            ('sprite_extended', 0x2000, 0x7FFF),
        ]
        for name, lo, hi in ranges:
            print(f'\n=== {name} ${lo:04x}-${hi:04x} ===')
            r = cmd(sock, f, f'find_first_divergence wram {lo:x} {hi:x} 32')
            if not r.get('ok'):
                print(f'  err: {r.get("error")}'); continue
            if r.get('match', True):
                print(f'  matches'); continue
            addr = int(r['first_diff'], 16)
            rb = int(r['recomp'], 16)
            ob = int(r['oracle'], 16)
            print(f'  first diff: addr=$0x{addr:04x} '
                  f'recomp=0x{rb:02x} oracle=0x{ob:02x} '
                  f'(total {r.get("diff_count", "?")} bytes)')

        # Also print the labeled DP state ($00-$FF) where Bug #8
        # found $72.
        print('\n=== DP per-byte (where rec != emu) ===')
        rec_h = cmd(sock, f, 'dump_ram 0 256')['hex'].replace(' ', '')
        emu_h = cmd(sock, f, 'emu_read_wram 0 256')['hex'].replace(' ', '')
        rec_b = bytes.fromhex(rec_h)
        emu_b = bytes.fromhex(emu_h)
        diffs = [(i, rec_b[i], emu_b[i]) for i in range(256)
                 if rec_b[i] != emu_b[i]]
        # Skip raw scratch.
        for addr, rv, ov in diffs:
            note = ''
            if addr == 0x72: note = ' = PlayerInAir (Bug #8 seed!)'
            elif addr == 0x77: note = ' = PlayerBlockedDir'
            elif addr == 0x7d: note = ' = PlayerYSpeed'
            elif addr == 0x96 or addr == 0x97: note = ' = PlayerYPosNext'
            elif addr == 0xd3 or addr == 0xd4: note = ' = PlayerYPos'
            elif addr == 0xd1 or addr == 0xd2: note = ' = PlayerXPos'
            print(f'  ${addr:02x}: rec=${rv:02x} emu=${ov:02x}{note}')
        if not diffs:
            print('  (DP matches)')
    finally:
        try: sock.close()
        except Exception: pass
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill()
        _kill()


if __name__ == '__main__':
    main()
