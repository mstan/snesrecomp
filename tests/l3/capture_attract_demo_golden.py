"""Capture a golden WRAM fixture from the embedded snes9x oracle.

One-time capture script. Launches build/bin-x64-Oracle/smw.exe --paused,
advances the attract demo to a set of state-sync checkpoints, and
records ORACLE's WRAM for a curated set of player-state addresses.
Writes the snapshot to snesrecomp/tests/l3/fixtures/attract_demo_golden.json.

Why state-based (not frame-based) sync: oracle's snes9x takes ~400
frames to complete BIOS+reset; recomp memsets WRAM to 0 and enters
game code at frame 1. At any fixed wall-clock frame the two sides are
in DIFFERENT game states. Checkpoints defined as "N frames dwell-time
inside a target GameMode" put both sides on the same demo-script
progression — v2's core improvement over v1 which used absolute
frame numbers.

Why oracle is golden: the embedded snes9x interprets the unmodified
ROM directly. Its WRAM is ground truth for ROM-intended behavior.

Usage:
  python capture_attract_demo_golden.py

Re-run only when the fixture needs refreshing (intentional ROM or
runtime behavior change). Not part of normal test flow.
"""
from __future__ import annotations
import json
import pathlib
import socket
import subprocess
import sys
import time


REPO = pathlib.Path(r'F:/Projects/SuperMarioWorldRecomp')
EXE  = REPO / 'build/bin-x64-Oracle/smw.exe'
FIXTURE_PATH = REPO / 'snesrecomp/tests/l3/fixtures/attract_demo_golden.json'


# State-sync checkpoints. Each: (name, target_game_mode, dwell_frames).
# Advance until GameMode first equals target, then step dwell_frames more.
# Both recomp and oracle measure their own entry-frame separately, so
# they're compared at the same point in the demo script regardless of
# when each side actually reached the mode.
CHECKPOINTS = [
    ('title_dwell_30',   0x07, 30),
    ('title_dwell_90',   0x07, 90),
    ('title_dwell_180',  0x07, 180),
    ('title_dwell_300',  0x07, 300),
    ('title_dwell_480',  0x07, 480),
]

# Hard cap on how many frames we'll spend waiting for a mode transition
# before giving up (snes9x boot is slow; demo cycles can take a while).
MODE_WAIT_MAX_FRAMES = 2000


# Curated WRAM addresses to capture. Player state + scroll + game mode.
WATCH_ADDRS = [
    ('GameMode',                 0x0100, 1),
    ('PlayerAnimation',          0x0071, 1),
    ('PlayerInAir',              0x0072, 1),
    ('PlayerYSpeed_lo',          0x007c, 1),
    ('PlayerYSpeed_hi',          0x007d, 1),
    ('PlayerXSpeed_hi',          0x007b, 1),
    ('PlayerXPosNext',           0x0094, 2),
    ('PlayerYPosNext',           0x0096, 2),
    ('PlayerYPosInBlock',        0x0090, 1),
    ('PlayerBlockMoveY',         0x0091, 1),
    ('PlayerXPosInBlock',        0x0092, 1),
    ('PlayerBlockXSide',         0x0093, 1),
    ('TouchBlockYPos',           0x0098, 2),
    ('TouchBlockXPos',           0x009a, 2),
    ('Layer1ScrollX',            0x00d1, 2),
    ('Layer1ScrollY',            0x00d3, 2),
    ('PlayerDrawY',              0x13e0, 2),
    ('PlayerIsOnGround',         0x13ef, 1),
    ('PlayerStandingOnTileType', 0x1471, 1),
]


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


def read_game_mode_oracle(sock, f):
    """Read ORACLE's GameMode from the embedded snes9x. Critical: the
    capture syncs on oracle's timeline (not recomp's) so that each
    checkpoint's state is measured at a known point in oracle's own
    attract-demo script. Recomp and oracle are in DIFFERENT GameModes
    at the same wall-clock frame (recomp boots faster), so using the
    wrong side would poison the fixture."""
    r = cmd(sock, f, 'emu_read_wram 0x100 1')
    if not r.get('ok'):
        raise RuntimeError(f'emu_read_wram 0x100: {r}')
    return int(r['hex'], 16)


def advance_until_mode(sock, f, target_mode):
    """Step one frame at a time until ORACLE's GameMode first equals
    target_mode. Returns the frame number at which the match happened."""
    start_frame = cmd(sock, f, 'frame').get('frame', 0)
    for _ in range(MODE_WAIT_MAX_FRAMES):
        gm = read_game_mode_oracle(sock, f)
        if gm == target_mode:
            return cmd(sock, f, 'frame').get('frame', 0)
        step(sock, f, 1)
    raise RuntimeError(
        f'oracle GameMode never reached 0x{target_mode:02x} within '
        f'{MODE_WAIT_MAX_FRAMES} frames of start_frame {start_frame}'
    )


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

        # Walk once, capturing checkpoints in order. For each checkpoint:
        # advance to target GameMode (from wherever we are), then step
        # dwell_frames. We do NOT reset between checkpoints — each one
        # is relative to the first entry into its target mode, and if the
        # mode cycles we re-enter on re-advance. Checkpoints within a
        # single stay-in-mode are ordered by dwell_frames.
        fixture = {
            'rom': 'smw.sfc',
            'backend': backend,
            'schema_version': 2,
            'generator': 'capture_attract_demo_golden.py (v2)',
            'sync': 'state-based (wait_for_game_mode + dwell_frames)',
            'checkpoints': [],
        }

        # Simple strategy: for each checkpoint, advance to the target mode
        # (idempotent if already there), then step dwell_frames. To ensure
        # dwell is measured from a consistent base, we reset-advance to the
        # target on each checkpoint — cheap since once we're in the mode
        # it's a no-op.
        # NOTE: if you want multiple checkpoints to share a "first entry"
        # anchor (e.g., title_30 + title_90 both measured from same entry),
        # compute their dwell-deltas relative to the prior checkpoint.
        prev_mode = None
        prev_dwell = 0
        for name, target_mode, dwell in CHECKPOINTS:
            if prev_mode == target_mode:
                # Same mode as prior checkpoint — step the DELTA.
                step(sock, f, dwell - prev_dwell)
            else:
                # First checkpoint in this mode — advance to it then dwell.
                entry_frame = advance_until_mode(sock, f, target_mode)
                step(sock, f, dwell)
            frame_at_checkpoint = cmd(sock, f, 'frame').get('frame', 0)
            snap = snapshot_oracle(sock, f)
            print(f'{name}: GameMode={snap["GameMode"]["value"]} '
                  f'frame={frame_at_checkpoint} '
                  f'YPos={snap["PlayerYPosNext"]["value"]} '
                  f'XPos={snap["PlayerXPosNext"]["value"]}')
            fixture['checkpoints'].append({
                'name': name,
                'wait_for_game_mode': f'0x{target_mode:02x}',
                'dwell_frames': dwell,
                'captured_at_frame': frame_at_checkpoint,
                'state': snap,
            })
            prev_mode = target_mode
            prev_dwell = dwell
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
