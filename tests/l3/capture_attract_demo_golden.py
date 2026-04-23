"""Capture a golden WRAM fixture from the embedded snes9x oracle.

One-time capture script. Launches build/bin-x64-Oracle/smw.exe --paused,
steps through the attract demo, and records ORACLE's WRAM at specific
checkpoint frames for a curated set of player-state addresses. Writes
the snapshot to snesrecomp/tests/l3/fixtures/attract_demo_golden.json.

Why oracle: the embedded snes9x runs the unmodified ROM against its
own 65816 interpreter. Its WRAM at any frame is what the ROM intends.
Recomp's WRAM may differ — that difference IS the set of bugs.

Why fixed frames: recomp is deterministic from --paused + no joypad.
Same inputs → same state at frame N, every run. Oracle in the Oracle
build runs lock-step with recomp (one per RtlRunFrame in main.c), so
their frame counters agree.

Usage:
  python capture_attract_demo_golden.py

Re-run only when the fixture needs refreshing (e.g., intentional ROM
or runtime behavior change). Not part of normal test flow.
"""
from __future__ import annotations
import json
import pathlib
import socket
import subprocess
import sys
import time


# Hardcoded because `snesrecomp/` is a junction out of the parent repo,
# so pathlib.Path(__file__).resolve() dereferences through the junction
# and .parents loses track of the outer SuperMarioWorldRecomp root.
REPO = pathlib.Path(r'F:/Projects/SuperMarioWorldRecomp')
EXE  = REPO / 'build/bin-x64-Oracle/smw.exe'
FIXTURE_PATH = REPO / 'snesrecomp/tests/l3/fixtures/attract_demo_golden.json'


# Checkpoint frames. Spread across the attract-demo timeline. Low frames
# catch boot; later frames catch gameplay. Extend as investigations need.
CHECKPOINT_FRAMES = [100, 200, 300, 400, 500, 600, 800, 1000]


# Curated WRAM addresses to capture. Each entry: (name, address, width).
# Player state + scroll + game mode. Extend as new bugs surface.
WATCH_ADDRS = [
    ('GameMode',           0x0100, 1),
    ('PlayerAnimation',    0x0071, 1),
    ('PlayerInAir',        0x0072, 1),
    ('PlayerYSpeed_lo',    0x007c, 1),
    ('PlayerYSpeed_hi',    0x007d, 1),
    ('PlayerXSpeed_hi',    0x007b, 1),
    ('PlayerXPosNext',     0x0094, 2),
    ('PlayerYPosNext',     0x0096, 2),
    ('PlayerYPosInBlock',  0x0090, 1),
    ('PlayerBlockMoveY',   0x0091, 1),
    ('PlayerXPosInBlock',  0x0092, 1),
    ('PlayerBlockXSide',   0x0093, 1),
    ('TouchBlockYPos',     0x0098, 2),
    ('TouchBlockXPos',     0x009a, 2),
    ('Layer1ScrollX',      0x00d1, 2),
    ('Layer1ScrollY',      0x00d3, 2),
    ('PlayerDrawY',        0x13e0, 2),
    ('PlayerIsOnGround',   0x13ef, 1),
    ('PlayerStandingOnTileType', 0x1471, 1),
]


def cmd(sock, f, line):
    sock.sendall((line + '\n').encode())
    return json.loads(f.readline())


def step_and_wait(sock, f, n):
    cur = cmd(sock, f, 'frame').get('frame', 0)
    cmd(sock, f, f'step {n}')
    target = cur + n
    deadline = time.time() + 120
    while time.time() < deadline:
        r = cmd(sock, f, 'frame')
        if r.get('frame', 0) >= target:
            return r.get('frame', 0)
        time.sleep(0.02)
    return cmd(sock, f, 'frame').get('frame', 0)


def read_oracle(sock, f, addr, width):
    r = cmd(sock, f, f'emu_read_wram 0x{addr:x} {width}')
    if not r.get('ok'):
        raise RuntimeError(f'emu_read_wram 0x{addr:x} {width}: {r}')
    return bytes.fromhex(r['hex'])


def snapshot_oracle(sock, f):
    out = {}
    for name, addr, width in WATCH_ADDRS:
        b = read_oracle(sock, f, addr, width)
        if width == 1:
            v = b[0]
        elif width == 2:
            v = b[0] | (b[1] << 8)
        else:
            v = int.from_bytes(b, 'little')
        out[name] = {'addr': f'0x{addr:04x}', 'width': width, 'value': f'0x{v:0{width*2}x}'}
    return out


def main():
    if not EXE.exists():
        print(f'ERROR: {EXE} not found — build Oracle|x64 first', file=sys.stderr)
        return 1
    subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.5)
    proc = subprocess.Popen(
        [str(EXE), '--paused'],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        cwd=str(REPO),
    )
    try:
        sock = socket.socket()
        for _ in range(50):
            try: sock.connect(('127.0.0.1', 4377)); break
            except (ConnectionRefusedError, OSError): time.sleep(0.2)
        f = sock.makefile('r')
        banner = f.readline()
        if 'connected' not in banner:
            print(f'unexpected banner: {banner!r}', file=sys.stderr)
            return 1

        # Confirm oracle is active.
        r = cmd(sock, f, 'emu_list')
        if not r.get('ok') or not r.get('active'):
            print(f'no active oracle backend: {r}', file=sys.stderr)
            return 1
        backend = r.get('active')

        fixture = {
            'rom': 'smw.sfc',
            'backend': backend,
            'generator': 'capture_attract_demo_golden.py',
            'checkpoints': [],
        }

        for checkpoint in CHECKPOINT_FRAMES:
            frame_reached = step_and_wait(sock, f, checkpoint - (
                cmd(sock, f, 'frame').get('frame', 0) or 0))
            snap = snapshot_oracle(sock, f)
            print(f'frame {frame_reached}: {snap["GameMode"]["value"]} '
                  f'YPos={snap["PlayerYPosNext"]["value"]}')
            fixture['checkpoints'].append({
                'frame': frame_reached,
                'state': snap,
            })
        sock.close()
    finally:
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill()
        subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    FIXTURE_PATH.parent.mkdir(exist_ok=True)
    FIXTURE_PATH.write_text(json.dumps(fixture, indent=2))
    print(f'wrote {FIXTURE_PATH}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
