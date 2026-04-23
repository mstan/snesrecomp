"""Bug #8 — trace every function in the ROM call chain that should
reach $00EF6B STZ PlayerInAir on oracle. For each link, count
invocations during recomp's boot→mode-0x07 window. The first zero-hit
link is where recomp's flow diverges from oracle's.

Chain (oracle, derived from SMWDisX bank_00):
  GM04PrepTitleScreen (mode 4)
    -> GM12PrepLevel
       -> CODE_009860 (GameMode12_PrepareLevel_009860)
          -> ProcessPlayerAnimation (= GameMode14_InLevel_HandlePlayerState in US ROM)
             -> dispatch via $71==0 to PlayerState00 (= ResetAni)
                -> PlayerState00_00CCE0
                   -> CODE_00F8F2 (PlayerState00_00F8F2)  or CODE_00E92B
                      -> CODE_00EE1D (RunPlayerBlockCode_00EE1D)
                         -> JMP CODE_00EEE1 (RunPlayerBlockCode_00EEE1)
                            -> STZ $72 at $EF6B
"""
from __future__ import annotations
import json, pathlib, socket, subprocess, sys, time

REPO = pathlib.Path(r'F:/Projects/SuperMarioWorldRecomp')
EXE = REPO / 'build/bin-x64-Oracle/smw.exe'
MAX_BOOT_FRAMES = 2000
TARGET_MODE = 0x07

INTERESTING = [
    'GameMode04_PrepareTitleScreen', # ROM $9a8b — mode 4 handler
    'GameMode12_PrepareLevel',       # ROM $a59c — full-level prep subroutine
    'GameMode12_PrepareLevel_009860',# ROM $9860 — the subchain that calls animation dispatch
    'GameMode14_InLevel_HandlePlayerState',  # US ROM $c593 = ProcessPlayerAnimation
    'PlayerState00',                 # US ROM $cc68 = ResetAni entry
    'PlayerState00_00CCE0',          # Branch from PlayerState00 when EndLevelTimer=0
    'PlayerState00_00F8F2',          # cfg manual; CODE_00F8F2 entry
    'CODE_00E92B',                   # may not be in cfg; grep all traces
    'RunPlayerBlockCode_00EE1D',     # EE1D entry
    'RunPlayerBlockCode_00EEE1',     # EEE1 entry
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


def recomp_mode(sock, f):
    return int(cmd(sock, f, 'dump_ram 0x100 1')['hex'].replace(' ', ''), 16)


def main():
    subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.5)
    proc = subprocess.Popen([str(EXE), '--paused'],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, cwd=str(REPO))
    try:
        sock = socket.socket()
        for _ in range(50):
            try: sock.connect(('127.0.0.1', 4377)); break
            except (ConnectionRefusedError, OSError): time.sleep(0.2)
        f = sock.makefile('r')
        f.readline()

        cmd(sock, f, 'trace_calls_reset')
        cmd(sock, f, 'trace_calls')
        for frame in range(1, MAX_BOOT_FRAMES + 1):
            step1(sock, f)
            if recomp_mode(sock, f) == TARGET_MODE:
                print(f'recomp mode-0x07 at frame {frame}')
                break

        r = cmd(sock, f, 'get_call_trace')
        entries = r.get('log', [])
        print(f'total captured: {len(entries)}')
        counts = {n: 0 for n in INTERESTING}
        first_frames = {n: None for n in INTERESTING}
        for e in entries:
            fn = e.get('func', '') or ''
            for name in INTERESTING:
                if name in fn:
                    counts[name] += 1
                    if first_frames[name] is None:
                        first_frames[name] = e.get('f')
        print(f'\nchain hits (boot -> mode-0x07):')
        for name in INTERESTING:
            print(f'  {counts[name]:6d}  first=f{first_frames[name]}  {name}')

        # Same check for oracle's emu-side: no call trace on oracle.
        # But we can at least look at what recomp's chain actually
        # dispatched to. Show 30 unique function names that DID run
        # in depth 0-6 (outermost).
        unique = []
        seen = set()
        for e in entries:
            if e.get('d', 99) > 6: continue
            fn = e.get('func', '')
            if fn not in seen:
                seen.add(fn); unique.append((e.get('d'), fn))
                if len(unique) >= 60: break
        print(f'\nunique depth<=6 function names during boot (first 60):')
        for d, fn in unique:
            print(f'  d{d:2} {fn}')
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
