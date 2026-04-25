"""Trace the FULL function call chain from CheckPlayerToNormalSpriteCollision
during the koopa-contact window (f269-f275 per clipping data). Find what
fires immediately after CheckForContact at the moment of true overlap.
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
        cur = c.cmd('frame').get('frame', 0)
        if cur >= target: return cur
        time.sleep(0.05)
    return -1


def main():
    launch()
    c = DebugClient(RECOMP_PORT)
    try:
        c.cmd('pause')
        # Step to just before contact, arm trace_calls fresh, then step through
        step_to(c, 268)
        c.cmd('trace_calls_reset')
        c.cmd('trace_calls')
        step_to(c, 276)
        # Pull EVERY function call in this 8-frame window.
        r = c.cmd('get_call_trace from=268 to=276')
        log = r.get('log', [])
        print(f'Got {len(log)} calls in f268-276\n')
        # Print all
        for e in log:
            print(f'  f{e["f"]:4} d{e["d"]:3} {e["func"]:50} parent={e["parent"]}')
    finally:
        try: c.close()
        except Exception: pass
        _kill()


if __name__ == '__main__':
    main()
