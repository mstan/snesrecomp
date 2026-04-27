"""Issue B: walk back through TI values to find the EARLIEST
moment where any WRAM byte differs.

Strategy:
  1. Find every TI value emu has in its per-frame snapshot ring
     (covers 6000 frames; should include early demo phase).
  2. For each TI value (walking from earliest), find rec's
     matching frame from rec's $1df4 trace.
  3. Diff full $0000-$1FFF on both sides at the matched moment.
  4. Stop at the FIRST TI where diffs == 0 (or report all
     TIs as having diffs if even TI=$02 already differs).
  5. The TI WHERE DIFFS FIRST APPEAR (first TI > 0 diffs after
     a 0-diff TI) is the seed of divergence.
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

        # rec: trace $1df4 writes for the canonical TI list.
        rec_writes = cmd(sock, f, 'wram_writes_at 1df4 0 999999 4096').get('matches', [])
        rec_byval = {}
        for e in rec_writes:
            v = int(e['val'], 16) & 0xFF
            if v not in rec_byval: rec_byval[v] = int(e['f'])

        # emu: walk per-frame ring for every distinct $1df4 value.
        h = cmd(sock, f, 'emu_history')
        oldest = h.get('oldest', -1); newest = h.get('newest', -1)
        print(f'emu history: {oldest}..{newest}')
        emu_byval = {}
        prev_v = None
        for fi in range(oldest, newest + 1):
            r = cmd(sock, f, f'emu_wram_at_frame {fi} 1df4')
            if not r.get('ok'): continue
            v = int(r['val'], 16) & 0xFF
            if v != prev_v:
                if v not in emu_byval:
                    emu_byval[v] = fi
                prev_v = v

        common = sorted(set(rec_byval.keys()) & set(emu_byval.keys()))
        print(f'\nrec TI values: {sorted(rec_byval.keys())}')
        print(f'emu TI values: {sorted(emu_byval.keys())}')
        print(f'common TI values: {[hex(v) for v in common]}')

        # For each common TI, do a quick (low-WRAM) diff.
        def chunk_rec(rec_f, addr, length):
            r = cmd(sock, f, f'dump_frame_wram {rec_f} {addr:x} {length}')
            hex_str = r.get('hex', '').replace(' ', '')
            return bytes.fromhex(hex_str)

        for ti in common:
            rec_f = rec_byval[ti]
            emu_f = emu_byval[ti]
            print(f'\n=== TI=${ti:02x} (rec_frame={rec_f}, emu_frame={emu_f}) ===')

            # Read first 256 bytes (DP) from each side; quick check.
            rec_dp = chunk_rec(rec_f, 0, 256)
            emu_dp = bytearray(256)
            for off in range(256):
                rr = cmd(sock, f, f'emu_wram_at_frame {emu_f} {off:x}')
                emu_dp[off] = int(rr.get('val', '0xff'), 16) if rr.get('ok') else 0xff

            diffs = []
            for i in range(256):
                if rec_dp[i] != emu_dp[i] and i >= 0x20:  # skip lowest scratch
                    if rec_dp[i] == 0 and emu_dp[i] == 0x55:
                        continue  # init-policy
                    diffs.append((i, rec_dp[i], emu_dp[i]))
            print(f'  DP $20-$FF diffs: {len(diffs)}')
            for a, r, e in diffs[:8]:
                print(f'    ${a:02x}: rec=${r:02x} emu=${e:02x}')
            if len(diffs) > 8:
                print(f'    +{len(diffs) - 8} more')
    finally:
        try: sock.close()
        except Exception: pass
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill()
        _kill()


if __name__ == '__main__':
    main()
