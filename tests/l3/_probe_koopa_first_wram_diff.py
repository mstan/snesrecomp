"""Find the FIRST WRAM byte to diverge between recomp and oracle in
the attract demo. Uses the find_first_divergence TCP command which
scans bank-$7E WRAM byte-by-byte.

Workflow:
  1. Advance both sides to GM=0x07.
  2. Step both 1 frame in lockstep.
  3. Call find_first_divergence; report.
  4. Repeat for dwell 1..10 to see how the divergence propagates.

The EARLIEST divergent byte is the most upstream symptom — fix that
and many downstream divergences (XSpeed, YSpeed, position, GameMode)
collapse.
"""
from __future__ import annotations
import json, pathlib, socket, subprocess, time

REPO = pathlib.Path(r'F:/Projects/SuperMarioWorldRecomp')
EXE = REPO / 'build/bin-x64-Oracle/smw.exe'


def cmd(sock, f, line):
    sock.sendall((line + '\n').encode())
    return json.loads(f.readline())


def main():
    subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.5)
    proc = subprocess.Popen([str(EXE), '--paused'], cwd=REPO,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        sock = socket.socket()
        for _ in range(50):
            try: sock.connect(('127.0.0.1', 4377)); break
            except (ConnectionRefusedError, OSError): time.sleep(0.2)
        f = sock.makefile('r'); f.readline()

        # Advance both sides to GM=0x07
        for _ in range(2000):
            cmd(sock, f, 'step 1')
            if int(cmd(sock, f, 'dump_ram 0x100 1')['hex'].replace(' ',''),16) == 7:
                break
        for _ in range(2000):
            cmd(sock, f, 'emu_step 1')
            if int(cmd(sock, f, 'emu_read_wram 0x100 1')['hex'].replace(' ',''),16) == 7:
                break

        def report(dwell):
            # Use a tighter range that excludes recompiler scratch
            # ($00-$03) which legitimately differs because the harness
            # uses different DP for parameter passing. Real game state
            # starts at $04+.
            r = cmd(sock, f, 'find_first_divergence wram 4 1fff 32')
            if not r.get('ok'):
                print(f'  dwell={dwell:3}  err: {r.get("error")}')
                return
            if r.get('match', True):
                print(f'  dwell={dwell:3}  matches')
                return
            addr = int(r['first_diff'], 16)
            rb = int(r['recomp'], 16)
            ob = int(r['oracle'], 16)
            print(f'  dwell={dwell:3}  first diff: addr=0x{addr:04x} '
                  f'recomp=0x{rb:02x} oracle=0x{ob:02x} '
                  f'(total {r.get("diff_count", "?")} differing bytes)')

        print('=== both at GM=0x07 ===')
        report(0)
        for dwell in range(1, 30):
            cmd(sock, f, 'step 1')
            cmd(sock, f, 'emu_step 1')
            report(dwell)
    finally:
        sock.close()
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill()
        subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


if __name__ == '__main__':
    main()
