"""Find the FIRST HandlePlayerPhysics call where recomp's WRAM
diverges from oracle's. Uses the per-call snapshot ring to bisect
across hundreds of calls without re-running the demo.

Workflow:
  1. Arm snapshot rings on both sides (HandlePlayerPhysics).
  2. Run both sides forward enough frames to fill ring (200+ calls).
  3. For each call_idx 1..min(rec_count, ora_count):
     a. Fetch recomp slice + oracle slice.
     b. Diff $04+ (skip recomp scratch).
     c. Report first call with non-trivial diff.
"""
from __future__ import annotations
import json, pathlib, socket, subprocess, time

REPO = pathlib.Path(r'F:/Projects/SuperMarioWorldRecomp')
EXE  = REPO / 'build/bin-x64-Oracle/smw.exe'

RECOMP_FUNC_NAME = 'HandlePlayerPhysics'
ORACLE_PC24      = 0x00D5F2


def cmd(s, f, line):
    s.sendall((line + '\n').encode())
    return json.loads(f.readline())


def hb(r):
    return bytes.fromhex(r['hex'].replace(' ', ''))


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

        # Arm snapshots BEFORE any execution so we capture from frame 1.
        cmd(s, f, f'func_snap_set {RECOMP_FUNC_NAME}')
        cmd(s, f, f'emu_func_snap_set {ORACLE_PC24:x}')

        # Step both sides past attract-demo onset and into koopa contact.
        # Recomp dies around dwell ~262 in the prior probe; bisect
        # within the first 200 calls of HandlePlayerPhysics. With
        # ~1 call/frame, that's ~200 frames after GM=0x07.
        # Step both sides ~300 frames each. Use raw step counts;
        # snap rings hold the most recent 256.
        # Cap below the recomp-dies threshold (~frame 262 of attract demo).
        N = 240
        print(f'stepping both sides {N} frames...')
        for _ in range(N):
            cmd(s, f, 'step 1')
            cmd(s, f, 'emu_step 1')

        rc = cmd(s, f, 'func_snap_count').get('count', 0)
        oc = cmd(s, f, 'emu_func_snap_count').get('count', 0)
        print(f'snap counts: recomp={rc}, oracle={oc}')

        # Bisect per-call. Compare the smallest call_idx still in
        # both rings up to min(rc, oc).
        ring_len = 256
        lo = max(1, max(rc, oc) - ring_len + 1)
        hi = min(rc, oc)
        if hi < lo:
            print('no overlap window; rings exhausted before counts aligned')
            return

        def real_diff(i, rb, ob):
            if i < 4: return False
            if 0x100 <= i <= 0x1FF: return False
            if 0x200 <= i <= 0x4FF: return False
            return rb[i] != ob[i]

        print(f'tracking divergence growth across call_idx {lo}..{hi}...')
        for ci in list(range(1, 12)) + [lo+20, lo+40, hi-10, hi]:
            if ci < lo or ci > hi:
                continue
            rr = cmd(s, f, f'func_snap_get_n {ci} 0 8192')
            oo = cmd(s, f, f'emu_func_snap_get_n {ci} 0 8192')
            if not rr.get('ok') or not oo.get('ok'):
                continue
            rb = hb(rr); ob = hb(oo)
            n = min(len(rb), len(ob), 0x2000)
            diffs = [i for i in range(n) if real_diff(i, rb, ob)]
            top = diffs[:12]
            sample = ', '.join([f'0x{a:x}={rb[a]:02x}/{ob[a]:02x}' for a in top])
            print(f'  call={ci:3} (rf={rr.get("frame"):4} of={oo.get("frame"):4}): '
                  f'{len(diffs):3} diffs; first 12: {sample}')
    finally:
        s.close()
        p.terminate()
        try: p.wait(timeout=5)
        except subprocess.TimeoutExpired: p.kill()
        subprocess.run(['taskkill','/F','/IM','smw.exe'],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


if __name__ == '__main__':
    main()
