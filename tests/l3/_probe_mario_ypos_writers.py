"""Trace WRAM writes to PlayerYPos / PlayerYPosNext during f200->f300
(when recomp's Mario Y becomes +16 vs oracle). Surfaces the function
that incorrectly initializes Mario's Y position.

  $96-$97 = PlayerYPosNext (word)
  $D3-$D4 = PlayerYPosNow  (word)
"""
import sys, pathlib, time, subprocess, socket
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
    raise RuntimeError('no connect')


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
        step_to(c, 200)
        c.cmd('trace_wram_reset')
        # Watch both PlayerYPos slots (word writes touch +1 too).
        c.cmd('trace_wram 96 97')
        c.cmd('trace_wram d3 d4')
        step_to(c, 320)

        trace = c.cmd('get_wram_trace')
        log = trace.get('log', [])
        print(f'Captured {trace.get("entries", 0)} writes, showing all:')
        for e in log:
            a = int(e['adr'], 16)
            label = ('Ynext' if a in (0x96, 0x97) else 'Ynow' if a in (0xD3, 0xD4) else f'?{a:x}')
            print(f'  f{e["f"]:>3} ${a:04x} ({label}) val=0x{int(e["val"], 16):04x} w={e["w"]} fn={e["func"]} parent={e.get("parent", "?")}')
    finally:
        c.close(); _kill()


if __name__ == '__main__':
    main()
