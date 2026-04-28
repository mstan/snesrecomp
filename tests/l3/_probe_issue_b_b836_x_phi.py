"""Issue B: confirm X-phi gap at $0DB836.

Hypothesis: after `LDX $1` at $0DB847, the back-edge into label_b836
should refresh X. But the recomp emits `v14` for label_b836 and `v39`
for the LDX-load — they're never reconciled. So on the 2nd+ outer
iteration, the inner $3F loop reuses STALE X from the previous pass.

Proof: trace block-hooks at $0DB836. For each entry, compare X to
the X expected from the most recent $0DB847 (LDX $1). If the
trace shows label_b836's X != fresh X loaded at $0DB847 on a
back-edge entry, the bug is confirmed.

Even simpler: count $0DB836 block hits and inner-loop iterations.
ROM should iterate based on the freshly-loaded X each outer pass.
If recomp keeps the previous pass's X, the inner loop length is
wrong.
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


def _read_byte(sock, f, addr):
    h = cmd(sock, f, f'dump_ram 0x{addr:x} 1').get('hex', '').replace(' ', '')
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

        # Arm block trace BEFORE stepping. Step until level-load
        # is just done (GM=0x07), then immediately query — diagonal-
        # ledge runs during the load, so entries must be present.
        cmd(sock, f, 'trace_blocks')

        for _ in range(3000):
            cmd(sock, f, 'step 1')
            if _read_byte(sock, f, 0x100) == 0x07: break

        # Pull block trace entries for the diagonal-ledge function.
        # PC range covers $0DB7AA..$0DB84D.
        r = cmd(sock, f, 'get_block_trace pc_lo=0x0db7aa pc_hi=0x0db84d')
        entries = r.get('log', [])
        print(f'total block entries in DiagLedge: {len(entries)}')

        # Group by frame, then walk each call-instance.
        by_frame = {}
        for e in entries:
            by_frame.setdefault(e['f'], []).append(e)

        for fnum, evs in sorted(by_frame.items())[:1]:  # only first frame
            print(f'\n=== frame {fnum} ===')
            # Find each entry into the function, then dump the
            # block sequence with X values.
            for i, e in enumerate(evs):
                pc = int(e['pc'], 0)
                if pc in (0x0db7aa, 0x0db836, 0x0db82e, 0x0db823, 0x0db7d6):
                    print(f'  [{i:4}] pc={pc:06x} '
                          f'a={e["a"]:>8} x={e["x"]:>8} y={e["y"]:>8} '
                          f'd={e["d"]}')
                if i > 250:
                    break

        # Quick count: how many label_b836 hits per call-instance?
        b836 = sum(1 for e in entries if int(e['pc'], 0) == 0x0db836)
        b82e = sum(1 for e in entries if int(e['pc'], 0) == 0x0db82e)
        b7d6 = sum(1 for e in entries if int(e['pc'], 0) == 0x0db7d6)
        b7aa = sum(1 for e in entries if int(e['pc'], 0) == 0x0db7aa)
        print(f'\ntotal entries: b7aa={b7aa} b7d6={b7d6} '
              f'b836={b836} b82e={b82e}')
        print(f'  -> b836 = inner-loop counter test, b82e = $3F write body')
        print(f'  -> ratio b82e/b836 = {b82e/max(b836,1):.2f} '
              f'(should be ~(N-1)/N for N total iterations)')
    finally:
        try: sock.close()
        except Exception: pass
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill()
        _kill()


if __name__ == '__main__':
    main()
