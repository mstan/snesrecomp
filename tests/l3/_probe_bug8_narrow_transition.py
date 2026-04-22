"""Bug #8 phase 3: narrow the $04->$05 transition on emu to a single frame.

From phase 2 we know emu's $72 gets cleared somewhere between +180 and
+210 emu frames of extra runtime (the GameMode 4->5 boundary). This
probe walks emu one frame at a time across that window, logging each
transition of $100 (GameMode) and $72 (PlayerInAir). Outputs the
exact emu-frame where $72 flips 0x24 -> 0x00, plus CPU regs at that
frame.

Usage from repo root:
    python snesrecomp/tests/l3/_probe_bug8_narrow_transition.py
"""
import sys, pathlib, time, subprocess, socket
THIS_DIR = pathlib.Path(__file__).parent
sys.path.insert(0, str(THIS_DIR))
from harness import DebugClient, RECOMP_PORT  # noqa: E402

REPO = pathlib.Path(__file__).parent.parent.parent.parent
ORACLE_EXE = REPO / 'build' / 'bin-x64-Oracle' / 'smw.exe'

GAME_MODE = 0x100
PLAYER_IN_AIR = 0x72


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


def rb(c, cmd, addr, width=1):
    r = c.cmd(f'{cmd} 0x{addr:x} {width}')
    hex_s = r.get('hex', '').replace(' ', '')
    if not hex_s:
        return None
    b = bytes.fromhex(hex_s)
    return int.from_bytes(b[:width], 'little') if b else None


def step_to(c, target):
    cur = c.cmd('frame').get('frame', 0)
    if cur >= target:
        return cur
    c.cmd(f'step {target - cur}')
    deadline = time.time() + 60
    while time.time() < deadline:
        if c.cmd('frame').get('frame', 0) >= target:
            return target
        time.sleep(0.03)


def main():
    launch()
    c = DebugClient(RECOMP_PORT)
    try:
        c.cmd('pause')

        # Get recomp to f100 (GameMode=5, $72=0x24 — the bug).
        step_to(c, 100)

        # Coarse jump emu +150 to reach GameMode=4.
        c.cmd('emu_step 150')
        print(f'After +150 emu frames:')
        print(f'  emu GameMode = 0x{rb(c,"emu_read_wram",GAME_MODE):02x}, $72 = 0x{rb(c,"emu_read_wram",PLAYER_IN_AIR):02x}')
        print()
        print('Fine-grained sweep (1 emu frame at a time):')
        print(' emu_extra | GameMode | $72  | CPU regs (PC/A/X/Y)')
        print('-----------+----------+------+------------------')

        prev_mode = rb(c, 'emu_read_wram', GAME_MODE)
        prev_72   = rb(c, 'emu_read_wram', PLAYER_IN_AIR)

        # Walk 100 single-emu-frame steps past +150. Log every change.
        extra = 150
        for i in range(100):
            c.cmd('emu_step 1')
            extra += 1
            m = rb(c, 'emu_read_wram', GAME_MODE)
            v = rb(c, 'emu_read_wram', PLAYER_IN_AIR)
            if m != prev_mode or v != prev_72:
                r = c.cmd('emu_cpu_regs')
                pc = r.get('pc'); a = r.get('a'); x = r.get('x'); y = r.get('y')
                print(f'  +{extra:5d}  |   0x{m:02x}   | 0x{v:02x} | PC={pc} A={a} X={x} Y={y}')
                prev_mode = m; prev_72 = v
            if m == 0x05 and v == 0x00:
                # We've passed the event we care about.
                break

        c.cmd('continue')
    finally:
        c.close(); _kill()


if __name__ == '__main__':
    main()
