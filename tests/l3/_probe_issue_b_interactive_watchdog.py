"""Issue B interactive watchdog.

Connects to a RUNNING Release|x64 smw.exe (the live exe, no
oracle). Polls Mario's Y position. When Mario "sinks" — defined
as Y crossing below ground level by > 8 pixels (i.e. Y > Y_GROUND
+ 8 = $0158) for >= 3 consecutive samples — pauses the recomp
and dumps the most recent writers to $D3/$D4 from the always-on
WRAM-trace ring.

Usage:
  1. Launch the exe: ./build/bin-x64-Release/smw.exe
  2. In the game window, walk Mario toward the Yoshi ?-block.
  3. Run this probe in another terminal: python <this file>
  4. The probe attaches to the running TCP server on port 4377
     and watches Mario's Y. When Mario sinks at the bug site,
     it pauses + dumps the trace.

  No --paused flag, no oracle, just the live exe + Mario's eyes.
"""
from __future__ import annotations
import json, pathlib, socket, sys, time

PORT = 4377
GROUND_Y = 0x0150
SINK_THRESHOLD_Y = GROUND_Y + 8       # > this = "Mario underground"
CONSECUTIVE_REQUIRED = 3              # how many samples in a row to confirm
POLL_INTERVAL_S = 0.1                 # poll Mario state every 100ms


def cmd(sock, f, line):
    sock.sendall((line + '\n').encode())
    return json.loads(f.readline())


def _read_byte(sock, f, addr):
    h = cmd(sock, f, f'dump_ram 0x{addr:x} 1').get('hex', '').replace(' ', '')
    return int(h, 16) if h else -1


def _read_word(sock, f, addr):
    return (_read_byte(sock, f, addr + 1) << 8) | _read_byte(sock, f, addr)


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
    print('  attached. monitoring Mario Y...')
    print(f'  trigger: Y > ${SINK_THRESHOLD_Y:04x} for {CONSECUTIVE_REQUIRED}+ samples')
    print()

    consec = 0
    last_print = 0
    while True:
        rx = _read_word(sock, f, 0xD1)
        ry = _read_word(sock, f, 0xD3)
        gm = _read_byte(sock, f, 0x100)

        # Print status occasionally so the operator knows it's alive.
        now = time.time()
        if now - last_print > 2.0:
            print(f'  [t={now:.1f}] GM=${gm:02x} X=${rx:04x} Y=${ry:04x}')
            last_print = now

        if ry > SINK_THRESHOLD_Y and gm in (0x14, 0x15, 0x16):  # in-level
            consec += 1
            if consec >= CONSECUTIVE_REQUIRED:
                print(f'\n*** MARIO SINK DETECTED ***')
                print(f'  X=${rx:04x} Y=${ry:04x} (ground=${GROUND_Y:04x})')
                cmd(sock, f, 'pause')
                # Query writers to $D3 and $D4 (most recent).
                for addr in (0xD3, 0xD4):
                    r = cmd(sock, f, f'wram_writes_at {addr:x} 0 999999 30')
                    writes = r.get('matches', [])
                    print(f'\n--- last 20 writes to ${addr:02x} ---')
                    for e in writes[-20:]:
                        print(f'  f={e["f"]:5} val={e["val"]:>6} '
                              f'func={e["func"][:32]:32} '
                              f'parent={e["parent"][:25]}')
                # Player physics neighbors.
                print(f'\n--- player state at sink ---')
                for label, addr in [('YPos', 0xD3), ('YPosHi', 0xD4),
                                    ('YPosNext', 0x96), ('YPosNextHi', 0x97),
                                    ('YSpd', 0x7D), ('YSubSpd', 0x7E),
                                    ('OnGround', 0x13EF),
                                    ('PlayerInAir', 0x72),
                                    ('PlayerBlockedDir', 0x77),
                                    ('TempPlayerGround', 0x8D),
                                    ('PlayerBlockMoveY', 0x91),
                                    ('PlayerYPosInBlock', 0x90),
                                    ('Pose', 0x13E0)]:
                    v = _read_byte(sock, f, addr)
                    print(f'  ${addr:04x} {label:22} = ${v:02x}')
                print('\n[paused] use TCP `continue` to resume.')
                return
        else:
            consec = 0

        time.sleep(POLL_INTERVAL_S)


if __name__ == '__main__':
    main()
