"""Frozen koopa step 4: Tier 4 insn trace through ParseLevelSpriteList
slot-finder loop on recomp. Capture A/X/Y at each PC in $02a8df-$02a960
range during recomp's f95 spawn frame. Compare against oracle's
emu_insn_trace at the equivalent oracle moment."""
import json, pathlib, socket, subprocess, sys, time
REPO = pathlib.Path(r'F:/Projects/SuperMarioWorldRecomp')
EXE = REPO / 'build/bin-x64-Oracle/smw.exe'

def cmd(s, f, l):
    s.sendall((l + '\n').encode()); return json.loads(f.readline())
def step1(s, f):
    b = cmd(s, f, 'frame').get('frame', 0); cmd(s, f, 'step 1')
    dl = time.time() + 5
    while time.time() < dl:
        if cmd(s, f, 'frame').get('frame', 0) > b: return b + 1
        time.sleep(0.01)

def main():
    subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.5)
    p = subprocess.Popen([str(EXE), '--paused'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, cwd=str(REPO))
    try:
        s = socket.socket()
        for _ in range(50):
            try: s.connect(('127.0.0.1', 4377)); break
            except (ConnectionRefusedError, OSError): time.sleep(0.2)
        f = s.makefile('r'); f.readline()

        # --- recomp side: arm Tier 4 BEFORE f94 (since trace frame
        # labelling is pre-increment, we want the step that goes 94->95
        # which records as frame=94. So advance to counter=94, arm,
        # then 1 more step). Actually arm earlier and step several so
        # we capture the spawn regardless of off-by-one. ---
        for _ in range(90): step1(s, f)
        cmd(s, f, 'trace_insn_reset')
        cmd(s, f, 'trace_insn')
        for _ in range(8): step1(s, f)

        r = cmd(s, f, 'get_insn_trace pc_lo=0x02a8df pc_hi=0x02a960 limit=200')
        rec_log = r.get('log', [])
        print(f'recomp insns in $02a8df-$02a960 during spawn frame: {len(rec_log)}')
        for e in rec_log[:60]:
            print(f'  pc={e["pc"]} mnem={e["mnem"]:3} '
                  f'a={e["a"]:8} x={e["x"]:8} y={e["y"]:8}')

        # --- oracle side: catch up, arm emu_insn_trace at deep-mode-04 ---
        while int(cmd(s, f, 'emu_read_wram 0x100 1')['hex'].replace(' ',''), 16) != 0x04:
            cmd(s, f, 'emu_step 1')
        cmd(s, f, 'emu_insn_trace_reset')
        cmd(s, f, 'emu_insn_trace_on')
        # Step until oracle's slot 9 status flips to 0x01 (the spawn).
        for _ in range(200):
            cmd(s, f, 'emu_step 1')
            if int(cmd(s, f, 'emu_read_wram 0x14d1 1')['hex'].replace(' ',''), 16) != 0:
                break
        cmd(s, f, 'emu_insn_trace_off')

        # Pull oracle insns in same PC range.
        from_idx = 0
        ora_log = []
        total = cmd(s, f, 'emu_insn_trace_count').get('count', 0)
        while from_idx < total and len(ora_log) < 200:
            r = cmd(s, f, f'emu_get_insn_trace from={from_idx} limit=4096 pc_lo=0x02a8df pc_hi=0x02a960')
            batch = r.get('log', [])
            if not batch: break
            ora_log.extend(batch)
            from_idx = int(batch[-1]['i']) + 1
        print(f'\noracle insns in $02a8df-$02a960: {len(ora_log)}')
        for e in ora_log[:60]:
            print(f'  pc={e["pc"]} op={e["op"]} '
                  f'a={e["a"]:8} x={e["x"]:8} y={e["y"]:8}')
        return 0
    finally:
        try: s.close()
        except Exception: pass
        p.terminate()
        try: p.wait(timeout=5)
        except subprocess.TimeoutExpired: p.kill()
        subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

if __name__ == '__main__':
    sys.exit(main())
