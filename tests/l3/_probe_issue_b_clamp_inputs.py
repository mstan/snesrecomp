"""Issue B: trace the inputs to RunPlayerBlockCode_00EED1's clamp.

The clamp at $00:EED1 does:
    LDA $96 ; SBC $91 ; STA $96    ; Y_lo -= PlayerBlockMoveY
    LDA $97 ; SBC $90 ; STA $97    ; Y_hi -= PlayerYPosInBlock

If $90/$91 are computed using the WRONG tile row, the clamp
lands at $0160 (1 tile below ground) instead of $0150.

This probe queries the always-on ring for $90/$91 writes around
the sink-frame window (frames 745-755 from prior probe), and
prints the writer attribution + values. If $90/$91 reflect the
wrong tile, we find which routine computed them.
"""
from __future__ import annotations
import json, socket, sys
from collections import Counter

PORT = 4377
SINK_FRAME_START = 740
SINK_FRAME_END = 760


def cmd(sock, f, line):
    sock.sendall((line + '\n').encode())
    return json.loads(f.readline())


def main():
    sock = socket.socket()
    try:
        sock.connect(('127.0.0.1', PORT))
    except ConnectionRefusedError:
        print(f'no exe on port {PORT}'); sys.exit(1)
    f = sock.makefile('r')
    f.readline()

    print(f'querying $90, $91 writes in frame window {SINK_FRAME_START}-{SINK_FRAME_END}')
    for addr, label in [(0x90, 'PlayerYPosInBlock'),
                        (0x91, 'PlayerBlockMoveY')]:
        r = cmd(sock, f, f'wram_writes_at {addr:x} {SINK_FRAME_START} {SINK_FRAME_END} 4096')
        writes = r.get('matches', [])
        print(f'\n=== ${addr:02x} {label}: {len(writes)} writes in window ===')
        for e in writes[:60]:
            print(f'  f={e["f"]:5} val={e["val"]:>6} '
                  f'func={e["func"][:34]:34} '
                  f'parent={e["parent"][:25]}')

        # Group by writer + value to see the dominant pattern.
        by_func = Counter(e["func"] for e in writes)
        by_val = Counter(e["val"] for e in writes)
        print(f'  writers:')
        for fn, n in by_func.most_common(5):
            print(f'    {n:5}  {fn}')
        print(f'  values:')
        for v, n in by_val.most_common(8):
            print(f'    {n:5}  {v}')

    # Also dump $96/$97 in the same window for context.
    print(f'\n=== $96/$97 writes in window ===')
    for addr in (0x96, 0x97):
        r = cmd(sock, f, f'wram_writes_at {addr:x} {SINK_FRAME_START} {SINK_FRAME_END} 4096')
        writes = r.get('matches', [])
        print(f'  ${addr:02x}: {len(writes)} writes')
        for e in writes[:20]:
            print(f'    f={e["f"]:5} val={e["val"]:>6} func={e["func"][:34]}')


if __name__ == '__main__':
    main()
