"""Tier 2.5 WRAM-watchpoint demo + bug-#8 probe.

Arms a watchpoint on $72 (PlayerInAir). In recomp at f94+, the byte
stays 0x24. The oracle clears it to 0x00 somewhere f95..f100. Running
this probe against both runtimes tells us (a) whether recomp ever
writes 0x00 to $72 at all, and (b) if so, at which frame and from
which function. Same probe shape closes bug #9/#10 against other
WRAM bytes (swap the addr).

Usage from snesrecomp/ root:
    python snesrecomp/tests/l3/_probe_tier25_watch_72.py
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
        except OSError:
            time.sleep(0.2)
    raise RuntimeError('no connect')


def step_to(c, target):
    base = c.cmd('frame').get('frame', 0)
    if base >= target:
        return base
    c.cmd(f'step {target - base}')
    deadline = time.time() + 60
    while time.time() < deadline:
        if c.cmd('frame').get('frame', 0) >= target:
            return target
        time.sleep(0.05)


def rb(c, addr, n=1):
    r = c.cmd(f'read_ram 0x{addr:x} {n}')
    b = bytes.fromhex(r.get('hex', '').replace(' ', ''))
    if n == 1:
        return b[0] if b else None
    return int.from_bytes(b[:n], 'little') if b else None


def wait_parked(c, max_s=5.0):
    deadline = time.time() + max_s
    while time.time() < deadline:
        r = c.cmd('parked')
        if r.get('parked'):
            return r
        time.sleep(0.02)
    return None


def main():
    launch()
    c = DebugClient(RECOMP_PORT)
    try:
        c.cmd('pause')
        step_to(c, 94)

        # Confirm starting $72 value.
        start72 = rb(c, 0x72)
        print(f'f94 $72 = 0x{start72:02x}  (expect 0x24 — Mario in air)')

        # Watch $72 for value 0x00. The oracle clears it f95..f100.
        r = c.cmd('watch_add 72 0')
        print(f'watch_add $72==0: {r}')

        # Run several frames. If the clear happens, we'll get parked.
        target = 100
        c.cmd(f'step {target - 94}')

        # Poll: either we parked (clear happened) or we reached target f100.
        parked = None
        deadline = time.time() + 15
        while time.time() < deadline:
            p = c.cmd('parked')
            if p.get('parked'):
                parked = p; break
            fr = c.cmd('frame').get('frame', 0)
            if fr >= target:
                break
            time.sleep(0.05)

        if parked:
            print('\n*** WATCH HIT ***')
            print(f'  frame          = {c.cmd("frame").get("frame")}')
            print(f'  watch_addr     = {parked.get("watch_addr")}')
            print(f'  watch_val      = {parked.get("watch_val")}')
            print(f'  watch_width    = {parked.get("watch_width")}')
            print(f'  writing func   = {parked.get("writer")}')
            # Resume.
            c.cmd('watch_continue')
        else:
            fr = c.cmd('frame').get('frame', 0)
            cur72 = rb(c, 0x72)
            print(f'\nNo hit by f{fr}. $72 = 0x{cur72:02x}')
            print('This confirms the bug: recomp never clears $72 '
                  'between f94 and f{}. Expected the oracle-behaviour '
                  'writer to fire; it does not in recomp.'.format(fr))

        c.cmd('watch_clear')
        c.cmd('continue')
    finally:
        c.close(); _kill()


if __name__ == '__main__':
    main()
