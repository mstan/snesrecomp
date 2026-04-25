"""Compare recomp vs oracle WRAM at frame 0 (post-reset, before
any game logic runs). If they differ, the divergence is upstream
of any input — initialization mismatch.

Then step both 1 frame at a time, comparing each step. If the
divergence appears at frame N>0, we know the bug is in some
function called between frames N-1 and N.
"""
from __future__ import annotations
import json, pathlib, socket, subprocess, time

REPO = pathlib.Path(r'F:/Projects/SuperMarioWorldRecomp')
EXE = REPO / 'build/bin-x64-Oracle/smw.exe'


def cmd(s, f, line):
    s.sendall((line + '\n').encode())
    return json.loads(f.readline())


def main():
    subprocess.run(['taskkill','/F','/IM','smw.exe'],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.5)
    p = subprocess.Popen([str(EXE),'--paused'], cwd=REPO,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        s = socket.socket()
        for _ in range(50):
            try: s.connect(('127.0.0.1', 4377)); break
            except (ConnectionRefusedError, OSError): time.sleep(0.2)
        f = s.makefile('r'); f.readline()

        # Read frame counters from both sides.
        rf = cmd(s, f, 'frame')
        print(f'recomp at startup: {rf}')

        # Compare WRAM IMMEDIATELY (frame 0, before any step).
        r = cmd(s, f, 'find_first_divergence wram 4 1fff 32')
        print(f'\nfind_first_divergence at startup:')
        if r.get('match'):
            print('  MATCH: recomp + oracle have identical WRAM at startup')
        else:
            addr = int(r['first_diff'], 16)
            rb = int(r['recomp'], 16); ob = int(r['oracle'], 16)
            n = r.get('diff_count', '?')
            print(f'  DIVERGENT: first @0x{addr:04x} recomp=0x{rb:02x} oracle=0x{ob:02x}; total {n} differing bytes')

        # Step 1 frame each side; compare.
        cmd(s, f, 'step 1')
        cmd(s, f, 'emu_step 1')
        r = cmd(s, f, 'find_first_divergence wram 4 1fff 32')
        print(f'\nAfter 1 frame each:')
        if r.get('match'):
            print('  MATCH')
        else:
            addr = int(r['first_diff'], 16)
            rb = int(r['recomp'], 16); ob = int(r['oracle'], 16)
            n = r.get('diff_count', '?')
            print(f'  first @0x{addr:04x} recomp=0x{rb:02x} oracle=0x{ob:02x}; total {n} bytes')

        # Step ~30 more frames, watch how divergence grows.
        for nstep in (5, 10, 30, 60, 120, 200, 240):
            for _ in range(nstep):
                cmd(s, f, 'step 1'); cmd(s, f, 'emu_step 1')
            r = cmd(s, f, 'find_first_divergence wram 4 1fff 32')
            n = r.get('diff_count', '?')
            mode_r = int(cmd(s, f, 'dump_ram 0x100 1')['hex'].replace(' ',''),16)
            mode_o = int(cmd(s, f, 'emu_read_wram 0x100 1')['hex'].replace(' ',''),16)
            tf_r = int(cmd(s,f,'dump_ram 0x13 1')['hex'].replace(' ',''),16)
            tf_o = int(cmd(s,f,'emu_read_wram 0x13 1')['hex'].replace(' ',''),16)
            print(f'  +{nstep:3} frames: {n} diffs; '
                  f'GM r=0x{mode_r:02x} o=0x{mode_o:02x}; '
                  f'TrueFrame r={tf_r} o={tf_o}')
    finally:
        s.close()
        p.terminate()
        try: p.wait(timeout=5)
        except subprocess.TimeoutExpired: p.kill()
        subprocess.run(['taskkill','/F','/IM','smw.exe'],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


if __name__ == '__main__':
    main()
