"""Bug #8 task #8 — bisect EB77 path divergence.

Arm trace_blocks AFTER stepping past boot (f0-f94), capture frame 95
only, filter output to PC range $EB77-$EF99 (EB77's full body). The
sequence of blocks gives recomp's exact flow through EB77 at mode-0x04
entry. Compare to oracle's sequence:
  EDF7 -> EE11 -> EE3A -> EED1 -> EEE1 -> EF05 -> EF38 -> EF60 -> STZ $72

The first block that diverges from oracle's sequence is where the
codegen-path bug lives."""
from __future__ import annotations
import json, pathlib, socket, subprocess, sys, time

REPO = pathlib.Path(r'F:/Projects/SuperMarioWorldRecomp')
EXE = REPO / 'build/bin-x64-Oracle/smw.exe'


def cmd(s, f, l):
    s.sendall((l + '\n').encode())
    return json.loads(f.readline())


def step1(s, f):
    b = cmd(s, f, 'frame').get('frame', 0)
    cmd(s, f, 'step 1')
    dl = time.time() + 5
    while time.time() < dl:
        if cmd(s, f, 'frame').get('frame', 0) > b:
            return b + 1
        time.sleep(0.01)


def main():
    subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.5)
    p = subprocess.Popen([str(EXE), '--paused'],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, cwd=str(REPO))
    try:
        s = socket.socket()
        for _ in range(50):
            try: s.connect(('127.0.0.1', 4377)); break
            except (ConnectionRefusedError, OSError): time.sleep(0.2)
        f = s.makefile('r'); f.readline()

        # Advance to counter=95 quietly. EB77 fires during the step that
        # labels frame=95 = the step that advances counter from 95 to 96.
        for _ in range(95):
            step1(s, f)
        print(f'after 95 steps, at frame: {cmd(s, f, "frame").get("frame")}')

        # Arm blocks early (at frame 0) so we capture the whole boot, then
        # filter output by frame=95 only. Reset first to be safe.
        cmd(s, f, 'trace_blocks_reset')
        cmd(s, f, 'trace_blocks')

        # Since trace_blocks is armed AT frame 95 already (we advanced
        # 95 steps), any blocks from here forward will record with
        # frame=95. Run one more step.
        step1(s, f)
        print(f'after step1, at frame: {cmd(s, f, "frame").get("frame")}')

        # Show frame distribution first.
        r = cmd(s, f, 'get_block_trace')
        log = r.get('log', [])
        from collections import Counter
        frames = Counter(e.get('f') for e in log)
        print(f'\nframe distribution in ring (total returned {len(log)}, ring entries {r.get("entries")}):')
        for fr, c in sorted(frames.items()):
            print(f'  f{fr}: {c}')
        r = cmd(s, f, 'get_block_trace from=95 to=95 pc_lo=eb77 pc_hi=ef99')
        log = r.get('log', [])
        print(f'\n{len(log)} blocks in PC $EB77-$EF99 during frame 95')
        for e in log:
            print(f'  d{e.get("d"):2} pc={e.get("pc")} a={e.get("a","?"):6} '
                  f'x={e.get("x","?"):6} y={e.get("y","?"):6} func={e.get("func")}')
        # Also sanity-check: how many blocks total fired in f95 (= this step)?
        r = cmd(s, f, 'get_block_trace from=95 to=95')
        print(f'\ntotal blocks in this step: emitted={r.get("emitted")}')
        # And: pull blocks in a broader EEx / EFx window.
        r = cmd(s, f, 'get_block_trace from=95 to=95 pc_lo=ea00 pc_hi=efff')
        log = r.get('log', [])
        print(f'\n{len(log)} blocks in PC $EA00-$EFFF during f95 (first 30):')
        for e in log[:30]:
            print(f'  d{e.get("d"):2} pc={e.get("pc")} func={e.get("func")}')
        # Dump unique PC prefixes to see what banks+pages were hit.
        r = cmd(s, f, 'get_block_trace from=95 to=95')
        log = r.get('log', [])
        prefixes = set()
        for e in log:
            prefixes.add(e.get('pc', '')[:6])  # e.g. "0x0093"
        print(f'\nunique PC prefixes in f95 trace ({len(prefixes)}):')
        for p in sorted(prefixes):
            print(f'  {p}')
        return 0
    finally:
        try: s.close()
        except Exception: pass
        p.terminate()
        try: p.wait(timeout=5)
        except subprocess.TimeoutExpired: p.kill()
        subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


if __name__ == '__main__':
    sys.exit(main())
