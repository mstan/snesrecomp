"""Integration test: recomp attract-demo state vs oracle-recorded golden.

v2 — state-based checkpoint sync.

Launches build/bin-x64-Release/smw.exe --paused, advances the attract
demo to each checkpoint's (GameMode, dwell_frames) target, and reads
recomp's WRAM. Compares against the golden fixture captured from the
Oracle build. A divergence IS a bug — state-sync puts both sides at
the same point in the attract-demo script regardless of boot-timing.

Sync semantics: checkpoint is `(wait_for_game_mode=X, dwell_frames=N)`.
Each side separately steps until its own GameMode first equals X, then
steps N more frames, then captures state. Since SMW's attract demo is
gated by "frames-since-entering-current-mode" (not absolute time),
both sides at dwell=N in mode=X are at the same frame of the demo
script.

Exit 0 on match, 1 on any bug-level divergence. No more "v1 exploratory"
noise — v2 is a real pass/fail regression gate.

Bug #8 shows as: `title_dwell_90 PlayerYPosNext (oracle=0x0150, recomp=?)`.

Usage:
  python test_attract_demo_golden.py
"""
from __future__ import annotations
import json
import pathlib
import socket
import subprocess
import sys
import time


REPO = pathlib.Path(r'F:/Projects/SuperMarioWorldRecomp')
EXE  = REPO / 'build/bin-x64-Release/smw.exe'
FIXTURE_PATH = REPO / 'snesrecomp/tests/l3/fixtures/attract_demo_golden.json'

MODE_WAIT_MAX_FRAMES = 2000


def cmd(sock, f, line):
    sock.sendall((line + '\n').encode())
    return json.loads(f.readline())


def step(sock, f, n):
    if n <= 0:
        return cmd(sock, f, 'frame').get('frame', 0)
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


def read_recomp_game_mode(sock, f):
    r = cmd(sock, f, 'dump_ram 0x100 1')
    return int(r['hex'].replace(' ', ''), 16)


def advance_until_mode(sock, f, target_mode):
    start_frame = cmd(sock, f, 'frame').get('frame', 0)
    for _ in range(MODE_WAIT_MAX_FRAMES):
        gm = read_recomp_game_mode(sock, f)
        if gm == target_mode:
            return cmd(sock, f, 'frame').get('frame', 0)
        step(sock, f, 1)
    raise RuntimeError(
        f'recomp GameMode never reached 0x{target_mode:02x} within '
        f'{MODE_WAIT_MAX_FRAMES} frames of start_frame {start_frame}'
    )


def read_recomp(sock, f, addr, width):
    r = cmd(sock, f, f'dump_ram 0x{addr:x} {width}')
    hex_str = r.get('hex', '').replace(' ', '')
    return bytes.fromhex(hex_str)


def snapshot_recomp(sock, f, state_schema):
    out = {}
    for name, info in state_schema.items():
        addr = int(info['addr'], 16)
        width = info['width']
        b = read_recomp(sock, f, addr, width)
        if width == 1:
            v = b[0]
        elif width == 2:
            v = b[0] | (b[1] << 8)
        else:
            v = int.from_bytes(b, 'little')
        out[name] = f'0x{v:0{width*2}x}'
    return out


def main():
    if not EXE.exists():
        print(f'ERROR: {EXE} not found — build Release|x64 first', file=sys.stderr)
        return 1
    if not FIXTURE_PATH.exists():
        print(f'ERROR: {FIXTURE_PATH} not found — run capture_attract_demo_golden.py',
              file=sys.stderr)
        return 1

    fixture = json.loads(FIXTURE_PATH.read_text())
    if fixture.get('schema_version') != 2:
        print(f'ERROR: fixture is schema v{fixture.get("schema_version")} '
              f'— re-capture with v2', file=sys.stderr)
        return 1
    checkpoints = fixture['checkpoints']
    if not checkpoints:
        print(f'ERROR: fixture has no checkpoints', file=sys.stderr)
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

    failures = []
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

        prev_mode = None
        prev_dwell = 0
        for cp in checkpoints:
            name = cp['name']
            target_mode = int(cp['wait_for_game_mode'], 16)
            dwell = cp['dwell_frames']
            # Mirror capture's "same mode → delta step, different mode → advance+step"
            if prev_mode == target_mode:
                step(sock, f, dwell - prev_dwell)
            else:
                advance_until_mode(sock, f, target_mode)
                step(sock, f, dwell)
            frame_at_checkpoint = cmd(sock, f, 'frame').get('frame', 0)
            golden_state = {k: v['value'] for k, v in cp['state'].items()}
            recomp_state = snapshot_recomp(sock, f, cp['state'])
            for addr_name in golden_state:
                expected = golden_state[addr_name]
                actual = recomp_state[addr_name]
                if expected != actual:
                    failures.append(
                        f'{name} (recomp@f{frame_at_checkpoint}) '
                        f'{addr_name} oracle={expected} recomp={actual}'
                    )
            prev_mode = target_mode
            prev_dwell = dwell
        sock.close()
    finally:
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill()
        subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    if failures:
        print(f'FAIL: {len(failures)} divergences from oracle-golden:')
        for msg in failures:
            print(f'  {msg}')
        return 1
    print(f'OK: recomp matches oracle golden at all {len(checkpoints)} '
          f'state-sync checkpoints.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
