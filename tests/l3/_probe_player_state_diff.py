"""Wide WRAM diff at f100, focusing on player-physics state.

Goal: find which WRAM byte oracle has set differently from recomp at
boot/level-load that drives the PlayerIsOnGround=0 vs 1 divergence.
"""
import sys, pathlib, time, subprocess, socket
THIS_DIR = pathlib.Path(__file__).parent
sys.path.insert(0, str(THIS_DIR))
from harness import RECOMP_EXE, ORACLE_EXE, RECOMP_PORT, ORACLE_PORT, DebugClient  # noqa: E402


def _kill(): subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'], capture_output=True)


def _ports_ready():
    for p in (RECOMP_PORT, ORACLE_PORT):
        try:
            s = socket.create_connection(('127.0.0.1', p), timeout=0.3); s.close()
        except OSError: return False
    return True


def launch_both():
    _kill(); time.sleep(0.5)
    subprocess.Popen([str(RECOMP_EXE), '--paused'],
        cwd=str(RECOMP_EXE.parent.parent.parent),
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.Popen([str(ORACLE_EXE), '--paused', '--theirs'],
        cwd=str(ORACLE_EXE.parent.parent.parent),
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    deadline = time.time() + 15
    while time.time() < deadline:
        if _ports_ready(): time.sleep(0.3); return
        time.sleep(0.2)


def step_to(c, target):
    base = c.cmd('frame').get('frame', 0)
    if base >= target: return base
    c.cmd(f'step {target - base}')
    deadline = time.time() + 60
    while time.time() < deadline:
        if c.cmd('frame').get('frame', 0) >= target: return target
        time.sleep(0.05)


def rb(c, addr, n):
    r = c.cmd(f'read_ram 0x{addr:x} {n}')
    return bytes.fromhex(r.get('hex', '').replace(' ', ''))


def main():
    launch_both()
    r = DebugClient(RECOMP_PORT); o = DebugClient(ORACLE_PORT)
    try:
        r.cmd('pause'); o.cmd('pause')
        F = 100
        step_to(r, F); step_to(o, F)
        # Dump $00-$1FFF (low WRAM, all DP and zero-page state).
        rb_data = rb(r, 0x0000, 0x2000)
        ob_data = rb(o, 0x0000, 0x2000)
        print(f'=== WRAM $0000-$1FFF byte-diff at f{F} ===')
        diffs = []
        for i in range(min(len(rb_data), len(ob_data))):
            if rb_data[i] != ob_data[i]:
                diffs.append((i, rb_data[i], ob_data[i]))
        print(f'Total diffs: {len(diffs)}')
        for a, rv, ov in diffs[:80]:
            print(f'  ${a:04x}  recomp=0x{rv:02x}  oracle=0x{ov:02x}')
    finally:
        r.close(); o.close(); _kill()


if __name__ == '__main__':
    main()
