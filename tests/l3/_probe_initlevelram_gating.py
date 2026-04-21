"""Compare the gating vars that decide Mario-init path at level-load:
  - ShowMarioStart      $141D
  - SublevelCount       $141A
  - DisableNoYoshiIntro $141F
  - SkipMidwayCastleIntro $13CF

If any of these differs at f93 (frame just before level-load runs),
that's why oracle takes the STZ-PlayerInAir path and recomp takes the
LDA #$24 path. Probe at f90, f93, f100.
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


def rb(c, addr):
    r = c.cmd(f'read_ram 0x{addr:x} 1')
    b = bytes.fromhex(r.get('hex', '').replace(' ', ''))
    return b[0] if b else None


def main():
    launch_both()
    r = DebugClient(RECOMP_PORT); o = DebugClient(ORACLE_PORT)
    try:
        r.cmd('pause'); o.cmd('pause')
        addrs = [
            (0x1471, 'StandOnSolidSprite'),
            (0x007D, 'PlayerYSpeed+1'),
            (0x0072, 'PlayerInAir'),
            (0x0077, 'PlayerBlockedDir'),
            (0x13EF, 'PlayerIsOnGround'),
        ]
        for f in [94, 95, 96, 100]:
            step_to(r, f); step_to(o, f)
            print(f'\n=== f{f} ===')
            for a, name in addrs:
                rv = rb(r, a); ov = rb(o, a)
                tag = '' if rv == ov else '  <<< DIFF'
                print(f'  {name:<24} ${a:04x}  R=0x{rv:02x}  O=0x{ov:02x}{tag}')
    finally:
        r.close(); o.close(); _kill()


if __name__ == '__main__':
    main()
