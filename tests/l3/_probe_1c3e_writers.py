"""Who writes $1C3E during real game f94-f96 end-to-end? With full Tier 1
coverage (all 9 banks wrapped), every writer in the real-game path is
captured by function name.
"""
import sys, pathlib, time, subprocess, socket
from collections import Counter
THIS_DIR = pathlib.Path(__file__).parent
sys.path.insert(0, str(THIS_DIR))
from harness import RECOMP_EXE, RECOMP_PORT, DebugClient  # noqa: E402


def _kill(): subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'], capture_output=True)


def launch():
    _kill(); time.sleep(0.5)
    subprocess.Popen([str(RECOMP_EXE), '--paused'],
        cwd=str(RECOMP_EXE.parent.parent.parent),
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    deadline = time.time() + 15
    while time.time() < deadline:
        try:
            s = socket.create_connection(('127.0.0.1', RECOMP_PORT), timeout=0.3); s.close(); return
        except OSError: time.sleep(0.2)


def step_to(c, target):
    base = c.cmd('frame').get('frame', 0)
    if base >= target: return base
    c.cmd(f'step {target - base}')
    deadline = time.time() + 60
    while time.time() < deadline:
        if c.cmd('frame').get('frame', 0) >= target: return target
        time.sleep(0.05)


def main():
    launch()
    c = DebugClient(RECOMP_PORT)
    try:
        c.cmd('pause')
        step_to(c, 80)
        c.cmd('trace_wram_reset')
        c.cmd('trace_wram 1c3e 1c40')     # the diverging word + next
        c.cmd('trace_wram 1be6 1ce5')     # full L1 buffer context
        step_to(c, 96)
        tr = c.cmd('get_wram_trace')
        log = tr.get('log', [])
        print(f'captured {tr.get("entries")} writes\n')

        # All writes to $1C3E, grouped by (frame, func)
        print('=== All writes to $1C3E ===')
        for e in log:
            a = int(e['adr'], 16)
            if a == 0x1C3E:
                print(f'  f{e["f"]} val=0x{e["val"]} w={e["w"]} func={e["func"]}')

        # Who writes L1VramBuffer ($1BE6-$1CE5)?
        funcs = Counter()
        for e in log:
            a = int(e['adr'], 16)
            if 0x1BE6 <= a <= 0x1CE5:
                funcs[e['func']] += 1
        print('\n=== L1VramBuffer writers (by count) ===')
        for f, n in funcs.most_common(15):
            print(f'  {n:>6}  {f}')
    finally:
        c.close(); _kill()


if __name__ == '__main__':
    main()
