"""Bug #8 phase 17: find which block on recomp leads into $EEE1
with Y=0x20. Trace all blocks in bank 00 $EE-$EF range, identify
the one immediately preceding each $EEE1 entry.
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

        c.cmd('trace_blocks_reset')
        c.cmd('trace_blocks')
        step_to(c, 215)

        # Pull every block in bank 00 $EE-$EF range.
        r = c.cmd('get_block_trace pc_lo=0xee00 pc_hi=0xf000')
        log = r.get('log', [])
        print(f'Blocks in $EE-$EF range, last 15 frames: {len(log)}')
        # Print each block with its regs.
        for e in log[-40:]:
            print(f'  f{e.get("f")} d{e.get("d")} PC={e.get("pc")} '
                  f'A={e.get("a")} X={e.get("x")} Y={e.get("y")} '
                  f'({e.get("func")})')

        # Also check if there's a call trace showing how EEE1 is entered.
        print()
        c.cmd('trace_calls_reset')
        c.cmd('trace_calls')
        step_to(c, 220)
        r = c.cmd('get_call_trace contains RunPlayerBlockCode_00EEE1')
        calls = r.get('log', [])
        print(f'EEE1 call trace (last 10):')
        for e in calls[-10:]:
            print(f'  f{e.get("f")} d{e.get("depth")} {e.get("func")} <- {e.get("parent")}')

        c.cmd('continue')
    finally:
        c.close(); _kill()


if __name__ == '__main__':
    main()
