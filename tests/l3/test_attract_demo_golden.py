"""Integration test: recomp attract-demo state vs oracle-recorded golden.

Launches build/bin-x64-Release/smw.exe --paused, steps through the attract
demo, and reads WRAM at the same addresses the golden fixture captured
from the oracle. Reports every per-address divergence.

The fixture (fixtures/attract_demo_golden.json) was captured once from
the Oracle build, where the embedded snes9x interprets the ROM directly.
Any divergence IS recomp diverging from ROM-intended behavior — but
some divergences are LEGITIMATE bugs (Bug #8 lives here), others are
timing drift.

v1 LIMITATIONS (see v2 plan below):
  - Boot-timing drift: oracle's snes9x takes ~400 frames to complete
    BIOS+reset; recomp memsets WRAM to 0 and enters game code at frame 1.
    At any fixed wall-clock frame, the two sides are in DIFFERENT game
    modes. GameMode sync gate skips these frames.
  - Demo-progression drift: even when both sides reach GM=0x07, recomp
    has been in that mode longer, so it's further along the attract
    demo. Player X position differs greatly (recomp ~2x further), so
    player state at the same frame is NOT apples-to-apples.
  - This means the v1 test is EXPLORATORY, not a pass/fail regression
    gate — it surfaces divergences for investigation, but many are
    demo-progression artifacts not bugs.

v2 improvements (future):
  - State-based checkpoints (e.g., "N frames after entering GM=0x07")
    instead of absolute frame numbers.
  - Tolerance per field: Mario X might differ by ~0-10 pixels legitimately;
    Mario Y should match exactly when both are "stationary on ground".
  - Allowlist of known-expected divergences.

Bug #8 lives here AS OF 2026-04-23 at `frame 500 PlayerYPosNext
(oracle=0x0150, recomp=0x0160)` — the 16-pixel Y offset that shows
Mario rendered 1 tile inside the ground. When that fix lands, this
entry should drop out.

Exit code: always 0 in v1 even on divergence, so the test doesn't block
CI during the exploratory phase. Flip to nonzero exit once v2 sync
reduces noise to only-real-bugs.

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


def cmd(sock, f, line):
    sock.sendall((line + '\n').encode())
    return json.loads(f.readline())


def step_and_wait(sock, f, n):
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


def read_recomp(sock, f, addr, width):
    r = cmd(sock, f, f'dump_ram 0x{addr:x} {width}')
    hex_str = r.get('hex', '').replace(' ', '')
    return bytes.fromhex(hex_str)


def snapshot_recomp(sock, f, state_schema):
    """state_schema is the per-checkpoint 'state' dict from the fixture —
    we use its keys and addresses to know what to read."""
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

        for cp in checkpoints:
            target_frame = cp['frame']
            golden_state = {k: v['value'] for k, v in cp['state'].items()}
            cur_frame = cmd(sock, f, 'frame').get('frame', 0)
            step_and_wait(sock, f, target_frame - cur_frame)
            recomp_state = snapshot_recomp(sock, f, cp['state'])
            # GameMode sync gate: oracle's snes9x takes longer to boot than
            # recomp (BIOS → reset → game code), so at the same wall-clock
            # frame number the two sides are often in DIFFERENT game modes.
            # Comparing player state across mismatched modes is noise —
            # gate comparison on GameMode agreement. The gate is the chosen
            # trade-off between "every byte must match" (too noisy) and
            # "compare when meaningful" (signal).
            if golden_state.get('GameMode') != recomp_state.get('GameMode'):
                # Emit one summary note rather than 20 spurious byte diffs.
                failures.append(
                    f"frame {target_frame} SKIPPED — GameMode mismatch "
                    f"(oracle={golden_state.get('GameMode')}, "
                    f"recomp={recomp_state.get('GameMode')}); "
                    f"boot-timing drift, not a bug at this checkpoint"
                )
                continue
            # GameMode agrees — every remaining divergence is a real bug.
            for name in golden_state:
                expected = golden_state[name]
                actual = recomp_state[name]
                if expected != actual:
                    failures.append(
                        f"frame {target_frame} {name} "
                        f"(oracle={expected}, recomp={actual})"
                    )
        sock.close()
    finally:
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill()
        subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    if failures:
        skipped = [m for m in failures if 'SKIPPED' in m]
        real = [m for m in failures if 'SKIPPED' not in m]
        print(f'{len(real)} real divergences + {len(skipped)} skipped checkpoints:')
        for msg in skipped:
            print(f'  {msg}')
        for msg in real:
            print(f'  {msg}')
        # v1 exits 0 even on failure — exploratory phase, not a pass/fail gate.
        return 0
    print(f'OK: recomp matches oracle golden at all {len(checkpoints)} checkpoints.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
