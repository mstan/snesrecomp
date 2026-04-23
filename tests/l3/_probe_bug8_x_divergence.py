"""Bug #8 task #9 — find the first instruction where X diverges.

Capture both sides' insn traces for the window around EB77->ED4A
execution. Side-by-side diff by PC sequence: where did oracle change
X to 0x08 but recomp keep it at 0?

Recomp: Tier-4 `trace_insn` + `get_insn_trace pc_lo=0xeb77 pc_hi=0xefff`
Oracle: snes9x `emu_insn_trace_on` + `emu_get_insn_trace pc_lo=... pc_hi=...`

Problem: oracle's $ED4A execution happens at oracle-frame 296, which
is ~204 emu-frames behind recomp. Both sides run lockstep in the
main loop, so recomp f95 = oracle f~-109 (oracle is still in its
early boot). We have to advance oracle separately via `emu_step`.

Approach:
  1. Advance to recomp counter=95 via `step 1` (both lockstep).
  2. Arm recomp trace_insn.
  3. Step 1 frame -> captures recomp's EB77 execution at frame=95.
  4. Record recomp trace snapshot.
  5. Advance oracle alone via emu_step until oracle reaches the
     moment just before its own $ED4A fires.
     - Oracle enters mode 0x04 at oracle-f241, hits $ED4A at f296.
     - So emu_step until oracle's frame==295, arm emu_insn_trace,
       emu_step a few more.
  6. Record oracle trace snapshot.
  7. Print both side-by-side."""
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
        if cmd(s, f, 'frame').get('frame', 0) > b: return b + 1
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

        # --- recomp capture ---
        for _ in range(95):
            step1(s, f)
        cmd(s, f, 'trace_insn_reset')
        cmd(s, f, 'trace_insn')
        step1(s, f)

        # Pull recomp insns from EB77 to ED4D (inclusive; covers the
        # JSR F44D at ED4A and the BNE/BEQ at ED4D).
        r_rec = cmd(s, f, 'get_insn_trace pc_lo=0xeb77 pc_hi=0xed4d limit=4096')
        rec_log = r_rec.get('log', [])
        print(f'recomp insns in $EB77-$ED4D: {len(rec_log)}')

        # --- oracle capture ---
        # Step oracle alone until oracle-frame near 295.
        # First, where IS oracle currently?
        # (There's no direct "emu frame" command; we can check $0100 to
        # see which GameMode and infer. Simpler: emu_step until $0100==4
        # and then a fixed number of emu-frames in.)
        # Keep emu_step'ing until oracle mode == 0x04.
        while int(cmd(s, f, 'emu_read_wram 0x100 1')['hex'].replace(' ', ''), 16) != 0x04:
            cmd(s, f, 'emu_step 1')
        # Now in oracle mode 0x04; step until just before mode transitions
        # to 0x05 — but arm insn trace now and capture 60 frames to be
        # safe.
        cmd(s, f, 'emu_insn_trace_reset')
        cmd(s, f, 'emu_insn_trace_on')
        # Step until oracle mode changes away from 0x04.
        for _ in range(100):
            cmd(s, f, 'emu_step 1')
            if int(cmd(s, f, 'emu_read_wram 0x100 1')['hex'].replace(' ', ''), 16) != 0x04:
                break
        cmd(s, f, 'emu_insn_trace_off')

        # Pull oracle's trace for the same PC window.
        from_idx = 0
        ora_log = []
        total = cmd(s, f, 'emu_insn_trace_count').get('count', 0)
        while from_idx < total and len(ora_log) < 500:
            r = cmd(s, f, f'emu_get_insn_trace from={from_idx} limit=4096 pc_lo=0xeb77 pc_hi=0xed4d')
            batch = r.get('log', [])
            if not batch: break
            ora_log.extend(batch)
            from_idx = int(batch[-1]['i']) + 1
        print(f'oracle insns in $EB77-$ED4D: {len(ora_log)}')

        # Side-by-side print: align by PC.
        print(f'\n{"recomp":^40} || {"oracle":^40}')
        print(f'{"pc      mnem a    x    y":40} || {"pc      op   a    x    y":40}')
        i = j = 0
        while i < len(rec_log) or j < len(ora_log):
            lr = rec_log[i] if i < len(rec_log) else None
            lo = ora_log[j] if j < len(ora_log) else None
            rs = f'{lr["pc"]} {lr["a"]:6} {lr["x"]:6} {lr["y"]:6}' if lr else ''
            os_ = f'{lo["pc"]} {lo["op"]} {lo["a"]:6} {lo["x"]:6} {lo["y"]:6}' if lo else ''
            mark = ''
            if lr and lo:
                if lr['pc'] == lo['pc']:
                    if lr['x'] != lo['x']:
                        mark = '  <-- X DIFF'
                    i += 1; j += 1
                elif int(lr['pc'], 16) < int(lo['pc'], 16):
                    i += 1; os_ = ''
                else:
                    j += 1; rs = ''
            elif lr: i += 1
            else: j += 1
            print(f'{rs:40} || {os_:40}{mark}')
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
