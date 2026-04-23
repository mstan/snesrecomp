"""Bug #8 — find the first frame at which recomp-WRAM diverges from
snes9x-oracle WRAM.

Both runtimes execute lock-step under the Oracle build (Release|x64 exe
contains no oracle). We step N frames, call `find_first_divergence
wram 0 0x1FF`, and report the (frame, address, recomp, oracle) tuple
of the first divergence in the low 512 bytes of bank 7E (gameplay scratch).

Strategy: coarse-to-fine bracket.
  Pass 1: step 30 frames at a time until any divergence appears.
  Pass 2: from (last_clean_frame), step 1 frame at a time until
          divergence reappears.

Output is a single "FIRST_DIVERGENCE frame=N addr=0x... recomp=... oracle=..."
line plus the surrounding context window from find_first_divergence.
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
RANGE_LO = 0x0000
RANGE_HI = 0x01FF      # low-page zero-page + DP scratch; Bug #8 lives here
COARSE_STEP = 30
MAX_FRAME   = 600


def cmd(sock, f, line):
    sock.sendall((line + '\n').encode())
    return json.loads(f.readline())


def step(sock, f, n):
    if n <= 0:
        return cmd(sock, f, 'frame').get('frame', 0)
    target = cmd(sock, f, 'frame').get('frame', 0) + n
    cmd(sock, f, f'step {n}')
    deadline = time.time() + 120
    while time.time() < deadline:
        r = cmd(sock, f, 'frame')
        if r.get('frame', 0) >= target:
            return r.get('frame', 0)
        time.sleep(0.02)
    return cmd(sock, f, 'frame').get('frame', 0)


def find_diff(sock, f, lo=RANGE_LO, hi=RANGE_HI):
    r = cmd(sock, f, f'find_first_divergence wram 0x{lo:x} 0x{hi:x} 8')
    if not r.get('ok'):
        print(f'ERROR: find_first_divergence: {r}')
        sys.exit(1)
    return r


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

        # Pass 1: coarse walk.
        last_clean = 0
        diverged_frame = None
        diverged_r = None
        for target in range(COARSE_STEP, MAX_FRAME + 1, COARSE_STEP):
            step(sock, f, target - cmd(sock, f, 'frame').get('frame', 0))
            cur = cmd(sock, f, 'frame').get('frame', 0)
            r = find_diff(sock, f)
            if r.get('match') is True:
                last_clean = cur
                print(f'f{cur:4d}  MATCH ({r.get("bytes_scanned")}b in 0x{RANGE_LO:03x}-0x{RANGE_HI:03x})')
                continue
            print(f'f{cur:4d}  DIFF  first={r["first_diff"]} r={r["recomp"]} o={r["oracle"]} count={r["diff_count"]}')
            diverged_frame = cur
            diverged_r = r
            break

        if diverged_frame is None:
            print(f'OK: no divergence in [{RANGE_LO:03x},{RANGE_HI:03x}] through frame {MAX_FRAME}')
            return 0

        # Relaunch paused to do the fine walk — we can't rewind in-place
        # (no Tier 3 anchor set up here), so bisect starts fresh.
        print(f'\nFine walk: relaunching paused, stepping from 0 to {diverged_frame-1} to bracket.')
        sock.close()
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill()
        subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(0.5)

        proc = subprocess.Popen(
            [str(EXE), '--paused'],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            cwd=str(REPO),
        )
        sock = socket.socket()
        for _ in range(50):
            try: sock.connect(('127.0.0.1', 4377)); break
            except (ConnectionRefusedError, OSError): time.sleep(0.2)
        f = sock.makefile('r')
        banner = f.readline()

        step(sock, f, last_clean)
        # Step 1 at a time until divergence.
        for frame in range(last_clean + 1, diverged_frame + 1):
            step(sock, f, 1)
            r = find_diff(sock, f)
            if r.get('match') is False:
                print(f'\nFIRST_DIVERGENCE frame={frame} '
                      f'addr={r["first_diff"]} recomp={r["recomp"]} oracle={r["oracle"]} '
                      f'diff_count={r["diff_count"]}')
                print('\nContext window:')
                print('  addr      r     o     diff')
                for e in r['context']:
                    mark = '<-- FIRST' if e['diff'] and int(e['adr'],16) == int(r['first_diff'],16) else ''
                    print(f'  {e["adr"]}  {e["r"]}  {e["o"]}  {e["diff"]!s:5} {mark}')
                return 0

        print('fine walk didn\'t reproduce divergence — boot-timing nondeterminism?')
        return 2

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
