"""Issue B history-query probe.

Don't WATCH for the sink — QUERY THE RING for it. The always-on
Tier-1 WRAM trace records every $D3/$D4 write since process
start with frame number, function, and parent. Run this probe
against any live exe (with or without playing) and it walks the
ring backward for sink-shaped events: writes that move Mario's Y
to a value below the canonical ground ($0150) at any frame in
recorded history.

Then dumps full attribution per sink event so we can identify
which function wrote the underground value, and what Mario's
state was just before / after.

This shape is correct per the project's "always consume ring
buffers, never time/attach to catch events" rule. The previous
poll-based watchdog was the wrong shape — events get caught by
querying history backward, not by attaching forward.
"""
from __future__ import annotations
import json, pathlib, socket, sys

PORT = 4377
GROUND_Y = 0x0150
SINK_THRESHOLD_Y = GROUND_Y          # any write moving Y past ground


def cmd(sock, f, line):
    sock.sendall((line + '\n').encode())
    return json.loads(f.readline())


def main():
    print(f'connecting to localhost:{PORT}...')
    sock = socket.socket()
    try:
        sock.connect(('127.0.0.1', PORT))
    except ConnectionRefusedError:
        print(f'  no exe on port {PORT}. Launch the live build first:')
        print(f'    ./build/bin-x64-Release/smw.exe')
        sys.exit(1)
    f = sock.makefile('r')
    f.readline()  # consume banner
    print('  attached. querying always-on ring backward for sink events...')

    # Pull the entire $D4 ($D3+1, the high byte of Mario's Y) trace.
    # When Y high goes above $01 (i.e., low ground = $0150), Mario is
    # below the visible play area. When Y goes from $0150 to a value
    # whose post-write low byte is > $50 with hi=$01, that's a sink-
    # shaped event.
    r = cmd(sock, f, 'wram_writes_at d3 0 999999 4096')
    d3_writes = r.get('matches', [])
    print(f'  $D3 writes recorded: {len(d3_writes)}')

    # Find sink-shaped events: Y low byte transitions from <= $50
    # (ground or above) to > $58 (1+ tile underground), with hi byte
    # constant at $01. Also accept word writes where the new value
    # is > $0158.
    sinks = []
    prev_lo = None
    for e in d3_writes:
        try:
            v = int(e.get('val', '0x0'), 16)
        except ValueError:
            continue
        # Word writes carry both bytes; byte writes carry just the
        # low byte. Use w field to disambiguate.
        if e.get('w') == 2:
            new_y = v & 0xFFFF
        else:
            # Byte write to $D3 = low byte change. Reconstruct using
            # the most recent observed low (best effort).
            new_y = 0x0100 + (v & 0xFF)  # assume Y hi = 0x01
        if new_y > SINK_THRESHOLD_Y + 8:
            sinks.append((e, new_y))

    # UpdateCurrentPlayerPositionRAM is a one-line copy ($D3 = $96).
    # The bug seed is upstream in $96 (PlayerYPosNext). Trace $96
    # writers and find sink events there too.
    r = cmd(sock, f, 'wram_writes_at 96 0 999999 4096')
    py_writes = r.get('matches', [])
    print(f'  $96 (PlayerYPosNext) writes recorded: {len(py_writes)}')

    py_sinks = []
    for e in py_writes:
        try: v = int(e.get('val', '0x0'), 16)
        except ValueError: continue
        if e.get('w') == 2:
            new_y = v & 0xFFFF
        else:
            new_y = 0x0100 + (v & 0xFF)
        if new_y > SINK_THRESHOLD_Y + 8:
            py_sinks.append((e, new_y))

    print(f'\n=== $96 sink-shaped writes (PlayerYPosNext > ${SINK_THRESHOLD_Y + 8:04x}): {len(py_sinks)} ===')
    from collections import Counter
    by_func_96 = Counter(e["func"] for e, y in py_sinks)
    by_parent_96 = Counter(e["parent"] for e, y in py_sinks)
    print(f'  by writer function:')
    for func, n in by_func_96.most_common(10):
        print(f'    {n:5}  {func}')
    print(f'  by parent function:')
    for par, n in by_parent_96.most_common(10):
        print(f'    {n:5}  {par}')

    # Find the FIRST sink-shaped write to $96 — that's the seed.
    if py_sinks:
        first_sink = py_sinks[0]
        print(f'\n=== FIRST sink-shaped $96 write ===')
        e, y = first_sink
        print(f'  frame {e["f"]}: $96 -> ${e["val"]} (Y~${y:04x})')
        print(f'  func={e["func"]}')
        print(f'  parent={e["parent"]}')
        print(f'  block_idx={e["bi"]}')

        # Show the 5 writes BEFORE this seed (what state led to it).
        first_idx = py_writes.index(e)
        prelude = py_writes[max(0, first_idx-5):first_idx+1]
        print(f'\n=== prelude: 5 writes to $96 immediately before the first sink ===')
        for ev in prelude:
            try: v = int(ev["val"], 16)
            except ValueError: v = -1
            print(f'  f={ev["f"]:5} val={ev["val"]:>6} '
                  f'func={ev["func"][:32]:32} '
                  f'parent={ev["parent"][:25]}')

    # $D3 sink summary too.
    print(f'\n=== $D3 sink summary ===')
    print(f'  total sink-shaped $D3 writes: {len(sinks)}')
    by_func_d3 = Counter(e["func"] for e, y in sinks)
    print(f'  by writer:')
    for func, n in by_func_d3.most_common(10):
        print(f'    {n:5}  {func}')


if __name__ == '__main__':
    main()
