"""Bug #8 phase 20: identify who calls PlayerState00 during GM06
(frames 201-209 on recomp), where the ROM wouldn't.
"""
import sys, pathlib, time, subprocess, socket
THIS_DIR = pathlib.Path(__file__).parent
sys.path.insert(0, str(THIS_DIR))
from harness import DebugClient, RECOMP_PORT  # noqa: E402

REPO = pathlib.Path(__file__).parent.parent.parent.parent
ORACLE_EXE = REPO / 'build' / 'bin-x64-Oracle' / 'smw.exe'


def _kill():
    subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'], capture_output=True)


def launch():
    _kill(); time.sleep(0.5)
    subprocess.Popen([str(ORACLE_EXE), '--paused'],
                     cwd=str(REPO),
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    deadline = time.time() + 20
    while time.time() < deadline:
        try:
            s = socket.create_connection(('127.0.0.1', RECOMP_PORT), timeout=0.3)
            s.close(); return
        except OSError:
            time.sleep(0.2)
    raise RuntimeError('no TCP connect')


def step_to(c, target):
    cur = c.cmd('frame').get('frame', 0)
    if cur >= target: return cur
    c.cmd(f'step {target - cur}')
    deadline = time.time() + 60
    while time.time() < deadline:
        if c.cmd('frame').get('frame', 0) >= target: return target
        time.sleep(0.03)


def main():
    launch()
    c = DebugClient(RECOMP_PORT)
    try:
        c.cmd('pause')

        c.cmd('trace_calls_reset')
        c.cmd('trace_calls')
        step_to(c, 210)

        # Filter for PlayerState00 calls within the GM06 window.
        r = c.cmd('get_call_trace from=200 to=210 contains=PlayerState00')
        log = r.get('log', [])
        print(f'PlayerState00-related calls in f200-f210 window: {len(log)}')
        # Print the ancestry: for each frame, who called PlayerState00 and
        # what was its parent?
        seen_keys = set()
        for e in log:
            f = e.get('f')
            func = e.get('func', '')
            parent = e.get('parent', '')
            depth = e.get('depth') or e.get('d')
            key = (func, parent)
            print(f'  f{f:4d} d{depth} {func} <- {parent}')

        # Also dump the raw call sequence at f205 (mid-window) at all depths
        # for the PlayerState00 call ancestry.
        print()
        print('Full call sequence at f205 (broadest filter):')
        r = c.cmd('get_call_trace from=205 to=205')
        log = r.get('log', [])
        # Show the depth-pyramid leading to PlayerState00.
        for e in log[:80]:
            print(f'  d{e.get("depth") or e.get("d")} {e.get("func")} <- {e.get("parent")}')

        c.cmd('continue')
    finally:
        c.close(); _kill()


if __name__ == '__main__':
    main()
