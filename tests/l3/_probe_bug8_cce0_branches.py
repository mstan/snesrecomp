"""Bug #8 — PlayerState00_00CCE0 branches on:
  - IRQNMICommand bit 7  (BPL -> CODE_00CD24)
  - IRQNMICommand bit 0  (LSR A; BCS -> CODE_00CD24)
  - IRQNMICommand bit 6  (BIT; BVS -> CODE_00CD1C)
  - PlayerInAir          (BNE -> CODE_00CD1C)

Check recomp state at frame 95 and see which branch fires. Compare to
oracle. Also count how many times each downstream function runs during
boot on recomp (PlayerState00_00F8F2, CODE_00E92B or equivalent,
CODE_00F94E, CODE_00F992 region, RunPlayerBlockCode_00EEE1)."""
from __future__ import annotations
import json, pathlib, socket, subprocess, sys, time

REPO = pathlib.Path(r'F:/Projects/SuperMarioWorldRecomp')
EXE = REPO / 'build/bin-x64-Oracle/smw.exe'


def cmd(sock, f, line):
    sock.sendall((line + '\n').encode())
    return json.loads(f.readline())


def step1(sock, f):
    b = cmd(sock, f, 'frame').get('frame', 0)
    cmd(sock, f, 'step 1')
    deadline = time.time() + 5
    while time.time() < deadline:
        if cmd(sock, f, 'frame').get('frame', 0) > b: return b + 1
        time.sleep(0.01)


def r_byte(sock, f, addr):
    r = cmd(sock, f, f'dump_ram 0x{addr:x} 1')
    return int(r['hex'].replace(' ', ''), 16)


def e_byte(sock, f, addr):
    r = cmd(sock, f, f'emu_read_wram 0x{addr:x} 1')
    return int(r['hex'].replace(' ', ''), 16) if r.get('ok') else -1


def main():
    # IRQNMICommand = $0DDA
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
        for _ in range(210):
            step1(sock, f)

        # Dump state at recomp f210 and matching oracle.
        print('=== recomp @ f210 ===')
        print(f'  $0DDA IRQNMICommand = 0x{r_byte(sock, f, 0xdda):02x}')
        print(f'  $0072 PlayerInAir   = 0x{r_byte(sock, f, 0x72):02x}')
        oframes = 0
        while e_byte(sock, f, 0x100) != 0x07 and oframes < 2000:
            cmd(sock, f, 'emu_step 1')
            oframes += 1
        print(f'=== oracle @ +{oframes} emu-only (mode 0x07 entry) ===')
        print(f'  $0DDA IRQNMICommand = 0x{e_byte(sock, f, 0xdda):02x}')
        print(f'  $0072 PlayerInAir   = 0x{e_byte(sock, f, 0x72):02x}')

        # Collect PlayerState00-descendant hits.
        from collections import Counter
        keys = [
            'PlayerState00_00CCE0',
            'PlayerState00_00F8F2',
            'PlayerState00_HandleEndOfLevel',
            'RunPlayerBlockCode_00EE1D',
            'RunPlayerBlockCode_00EEE1',
            'RunPlayerBlockCode_00EED1',
            'PlayerState00_00CD24',
            'PlayerState00_00CD1C',
            'CODE_00E92B',
            'CODE_00F94E',
            'CODE_00F992',
            'auto_00_CD24',
            'auto_00_CD1C',
            'auto_00_E92B',
            'auto_00_F94E',
            'auto_00_F992',
            'auto_00_F8F2',
        ]
        counts = Counter()
        first = {}
        for start in range(0, 220, 30):
            for q in keys:
                r = cmd(sock, f, f'get_call_trace from={start} to={start+35} contains={q}')
                for e in r.get('log', []):
                    fn = e.get('func', '')
                    if q in fn:
                        counts[fn] += 1
                        first.setdefault(fn, e.get('f'))
        print('\nhits on PlayerState00 descendants:')
        for fn, c in sorted(counts.items(), key=lambda x: -x[1]):
            print(f'  {c:4d}  f{first[fn]}  {fn}')
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
