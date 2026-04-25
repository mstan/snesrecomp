"""Capture Mario+sprite clipping rectangles during the attract demo by
tracing WRAM writes to $00-$0F (the rect storage). Passive, no break.

GetMarioClipping writes Mario's bounding box to $00-$03 + $08-$09.
GetSpriteClippingA writes sprite's bounding box to $04-$07 + $0A-$0B.
CheckForContact reads them.

If recomp's CheckForContact never returns "contact" but the game visually
shows Mario contacting the koopa, the rectangles must be wrong upstream.
"""
import sys, pathlib, time, subprocess, socket
THIS_DIR = pathlib.Path(__file__).parent
sys.path.insert(0, str(THIS_DIR))
from harness import RECOMP_EXE, RECOMP_PORT, DebugClient  # noqa: E402


def _kill():
    subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'], capture_output=True)


def launch():
    _kill(); time.sleep(0.5)
    subprocess.Popen([str(RECOMP_EXE), '--paused'],
        cwd=str(RECOMP_EXE.parent.parent.parent),
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    deadline = time.time() + 15
    while time.time() < deadline:
        try:
            s = socket.create_connection(('127.0.0.1', RECOMP_PORT), timeout=0.3)
            s.close(); return
        except OSError: time.sleep(0.2)
    raise RuntimeError('no connect')


def step_to(c, target):
    base = c.cmd('frame').get('frame', 0)
    if base >= target: return base
    c.cmd(f'step {target - base}')
    deadline = time.time() + 60
    while time.time() < deadline:
        try:
            cur = c.cmd('frame').get('frame', 0)
            if cur >= target: return cur
        except Exception:
            return -1
        time.sleep(0.05)
    return -1


def main():
    launch()
    c = DebugClient(RECOMP_PORT)
    try:
        c.cmd('pause')
        step_to(c, 268)
        c.cmd('trace_wram_reset')
        c.cmd('trace_wram 0 f')
        step_to(c, 270)

        trace = c.cmd('get_wram_trace')
        log = trace.get('log', [])
        # Filter to writes from CheckForContact's neighborhood
        # (GetMarioClipping / GetSpriteClippingA / SubGetMarioClipping).
        # Print every write to $00-$0F in temporal order, with writer.
        for e in log:
            f = e['f']; a = int(e['adr'], 16); v = int(e['val'], 16)
            fn = e.get('func', '?')
            print(f'  f{f:4} ${a:02x} = {v:02x}  ({fn})')

        # Also dump distinct funcs that wrote to $00-$0F in window.
        funcs = {}
        for e in log:
            fn = e.get('func', '?')
            funcs[fn] = funcs.get(fn, 0) + 1
        print(f'\nWriters to $00-$0F (frames 240-280):')
        for fn, n in sorted(funcs.items(), key=lambda x: -x[1])[:15]:
            print(f'  {n:5} {fn}')
    finally:
        try: c.close()
        except Exception: pass
        _kill()


if __name__ == '__main__':
    main()
