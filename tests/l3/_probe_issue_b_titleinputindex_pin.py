"""Issue B: pin on TitleInputIndex (the demo's own phase tracker)
to compare state at identical-input moments.

The attract demo reads from TitleScreenInputSeq[TitleInputIndex].
Both rec and emu read from the same ROM table with the same
TitleInputIndex value — i.e., when both have TitleInputIndex=N,
both are applying the SAME input byte. Any state divergence at
that point is the real bug.

For each TitleInputIndex value emu visits, find rec's matching
visit (via the always-on $1DF4 write trace) — at those paired
moments, diff full WRAM. The first byte that differs at a pair
of matched moments is the seed of Issue B.
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

        # Run the demo long enough for both sides to traverse
        # multiple TitleInputIndex values.
        for _ in range(3000):
            cmd(sock, f, 'step 1')
            if _read_byte(sock, f, 0x100, 'rec') == 0x07: break
        for _ in range(800):
            cmd(sock, f, 'step 1')

        # Pull both sides' $1DF4 (TitleInputIndex) write streams.
        rec_writes = cmd(sock, f, 'wram_writes_at 1df4 0 999999 4096').get('matches', [])
        emu_writes = cmd(sock, f, 'emu_wram_writes_at 1df4 0 999999 4096').get('matches', [])
        print(f'rec $1DF4 writes: {len(rec_writes)}')
        print(f'emu $1DF4 writes: {len(emu_writes)}')

        # Map each TitleInputIndex value to the recomp/emu frame
        # where it was set.
        rec_byval = {}    # val (int) -> first rec frame
        for e in rec_writes:
            v = int(e['val'], 16) & 0xFF
            if v not in rec_byval:
                rec_byval[v] = int(e['f'])

        emu_byval = {}
        for e in emu_writes:
            v = int(e['after'], 16) & 0xFF
            if v not in emu_byval:
                emu_byval[v] = int(e['f'])

        # Show the matched values both sides wrote, with the per-side
        # frame at which each was written.
        common = sorted(set(rec_byval.keys()) & set(emu_byval.keys()))
        print(f'\nTitleInputIndex values both sides have written:')
        print(f'  count = {len(common)}; values = {[hex(v) for v in common]}')
        print(f'\n  TI val | rec_frame | emu_frame')
        for v in common:
            print(f'  ${v:02x}    | {rec_byval[v]:5d}     | {emu_byval[v]:5d}')

        if not common:
            print('No common TitleInputIndex values yet; demo needs more steps.')
            return

        # Pick the LAST common value as the latest synchronized
        # demo moment — both sides have just transitioned to this
        # demo entry. Compare full DP-state at this moment.
        target = common[-1]
        rec_f = rec_byval[target]
        emu_f = emu_byval[target]
        print(f'\nSyncing on last-common TitleInputIndex=${target:02x}')
        print(f'  rec_frame={rec_f}, emu_frame={emu_f}')

        # Read rec WRAM RIGHT NOW (since the ring tells us its
        # most-recent state). Better: read state from rec at
        # rec_frame via debug_server's frame-history if available.
        # For now, use current state as a proxy and emu's
        # snapshot from emu_wram_at_frame.
        print()
        print(f'  state at TI=${target:02x} (rec=current, emu=history@{emu_f}):')
        print(f'  addr  rec    emu    diff')
        labels = [
            (0xD1, 'XLo'), (0xD2, 'XHi'),
            (0xD3, 'YLo'), (0xD4, 'YHi'),
            (0x94, 'XNextLo'), (0x95, 'XNextHi'),
            (0x96, 'YNextLo'), (0x97, 'YNextHi'),
            (0x7B, 'XSpd'), (0x7C, 'XSpdSub'),
            (0x7D, 'YSpd'), (0x7E, 'YSpdSub'),
            (0x72, 'PlayerInAir'), (0x77, 'PlayerBlockedDir'),
            (0x90, 'PlayerYInBlock'), (0x91, 'PlayerBlockMoveY'),
            (0x13E0, 'Pose'), (0x13EF, 'OnGround'),
            (0x100, 'GameMode'), (0x1422, 'LevelMode'),
        ]
        for addr, label in labels:
            r = _read_byte(sock, f, addr, 'rec')
            erec = cmd(sock, f, f'emu_wram_at_frame {emu_f} {addr:x}')
            e = int(erec.get('val', '0x0'), 16) if erec.get('ok') else -1
            mark = '  <- DIFF' if r != e else ''
            print(f'  ${addr:04x}  ${r:02x}    ${e:02x}    {mark}  {label}')
    finally:
        try: sock.close()
        except Exception: pass
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill()
        _kill()


if __name__ == '__main__':
    main()
