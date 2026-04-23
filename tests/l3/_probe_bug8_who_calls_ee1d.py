"""Bug #8 — did recomp ever execute CODE_00EE1D / EEE1 / EF60 between
frame 94 (InitializeLevelRAM) and frame 201 (mode-0x07 entry)?

Arms Tier 1.5 trace_calls to capture every RecompStackPush, then runs
the boot/title-load sequence up to mode-0x07. Filters the call trace
for any entry into the player-physics ground-handling chain and
reports each hit. If zero hits: Bug #8's root is "the call chain that
clears PlayerInAir isn't being entered on recomp during the pre-
mode-0x07 window."
"""
from __future__ import annotations
import json
import pathlib
import socket
import subprocess
import sys
import time


REPO = pathlib.Path(r'F:/Projects/SuperMarioWorldRecomp')
EXE = REPO / 'build/bin-x64-Oracle/smw.exe'
TARGET_MODE = 0x07
MAX_BOOT_FRAMES = 2000
# Function-name substrings we want hit counts for.
INTERESTING = [
    'RunPlayerBlockCode_00EE1D',
    'RunPlayerBlockCode_00EE3A',
    'RunPlayerBlockCode_00EE85',
    'RunPlayerBlockCode_00EED1',
    'RunPlayerBlockCode_00EEE1',
    'RunPlayerBlockCode_00EFBC',
    'RunPlayerBlockCode_00EFCD',
    'RunPlayerBlockCode_00EFE8',
    'RunPlayerBlockCode_00F005',
]


def cmd(sock, f, line):
    sock.sendall((line + '\n').encode())
    return json.loads(f.readline())


def step1(sock, f):
    before = cmd(sock, f, 'frame').get('frame', 0)
    cmd(sock, f, 'step 1')
    deadline = time.time() + 5
    while time.time() < deadline:
        if cmd(sock, f, 'frame').get('frame', 0) > before:
            return before + 1
        time.sleep(0.01)
    return before


def recomp_mode(sock, f):
    return int(cmd(sock, f, 'dump_ram 0x100 1')['hex'].replace(' ', ''), 16)


def main():
    if not EXE.exists():
        print(f'ERROR: Oracle exe not found at {EXE}', file=sys.stderr)
        return 1
    subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.5)
    proc = subprocess.Popen(
        [str(EXE), '--paused'],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
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

        r = cmd(sock, f, 'trace_calls_reset')
        r = cmd(sock, f, 'trace_calls')
        print(f'trace_calls armed: {r}')

        for frame in range(1, MAX_BOOT_FRAMES + 1):
            step1(sock, f)
            if recomp_mode(sock, f) == TARGET_MODE:
                print(f'recomp mode-0x07 at frame {frame}')
                break

        # Pull full call trace.
        r = cmd(sock, f, 'get_call_trace')
        entries = r.get('log', [])
        print(f'total call entries captured: {len(entries)}')

        counts = {name: 0 for name in INTERESTING}
        first_frames = {name: None for name in INTERESTING}
        for e in entries:
            fn = e.get('func', '') or ''
            for name in INTERESTING:
                if name in fn:
                    counts[name] += 1
                    if first_frames[name] is None:
                        first_frames[name] = e.get('f')

        print('\n=== hit counts during boot -> mode-0x07 ===')
        for name in INTERESTING:
            ff = first_frames[name]
            print(f'  {name:45s} {counts[name]:5d} hits  first_frame={ff}')

        # For anything that hit zero, also look for the DECODED name
        # variants or adjacent cfg-named functions.
        zeros = [n for n in INTERESTING if counts[n] == 0]
        if zeros:
            print('\n=== first 50 unique function names in trace (for cross-check) ===')
            unique_names = []
            seen = set()
            for e in entries:
                fn = e.get('func', '') or ''
                if fn not in seen:
                    seen.add(fn); unique_names.append(fn)
                    if len(unique_names) >= 50: break
            for n in unique_names:
                print(f'  {n}')
        return 0
    finally:
        try: sock.close()
        except Exception: pass
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill()
        subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


if __name__ == '__main__':
    sys.exit(main())
