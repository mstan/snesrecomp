"""Bug #8 phase 5: Tier-1 trace of every write to $72 during
recomp's GameMode-4 frame.

Since emu takes the STZ-PlayerInAir branch (writes $72=0x00) and
recomp takes the LDA#$24 / STA PlayerInAir branch (writes $72=0x24),
capturing recomp's write sequence tells us which function made the
call that landed on the wrong branch.

Usage from repo root:
    python snesrecomp/tests/l3/_probe_bug8_recomp_writer_trace.py
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


def rb(c, addr, n=1):
    r = c.cmd(f'read_ram 0x{addr:x} {n}')
    hex_s = r.get('hex', '').replace(' ', '')
    if not hex_s: return None
    return bytes.fromhex(hex_s)[0]


def main():
    launch()
    c = DebugClient(RECOMP_PORT)
    try:
        c.cmd('pause')

        # Step to f93 (just before GameMode hits 4).
        step_to(c, 93)
        print(f'f93: GameMode=0x{rb(c,0x100):02x}, $72=0x{rb(c,0x72):02x}')

        # Arm Tier-1 WRAM write trace for $72 (single byte). trace_wram
        # adds-and-activates the range in one call.
        c.cmd('trace_wram_reset')
        r = c.cmd('trace_wram 72 72')
        print(f'trace_wram: {r}')

        # Step through GameMode=4 frames (recomp spends only a few).
        # Walk to f102 to cover any post-transition writes too.
        step_to(c, 102)
        print(f'f102: GameMode=0x{rb(c,0x100):02x}, $72=0x{rb(c,0x72):02x}')

        # Fetch all writes to $72 captured in the trace.
        r = c.cmd('get_wram_trace')
        log = r.get('log', [])
        print(f'\nCaptured {len(log)} writes to $72 between f93 and f102:')
        for e in log:
            print(f'  f{e.get("f")}: '
                  f'$72 <- {e.get("val")}  '
                  f'width={e.get("w")}  '
                  f'func={e.get("func")!r}  '
                  f'parent={e.get("parent")!r}')

        c.cmd('continue')
    finally:
        c.close(); _kill()


if __name__ == '__main__':
    main()
