"""Tier 2.5 demo + bug-#8 probe: break at $00:$EBA5 (the WallRun-call
block inside EB77), dump intermediate state, then continue. Verifies
the pause/break/step machinery and gives us EB77's exact incoming
state at the divergent block.
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


def rb(c, addr, n=1):
    r = c.cmd(f'read_ram 0x{addr:x} {n}')
    b = bytes.fromhex(r.get('hex', '').replace(' ', ''))
    if n == 1: return b[0] if b else None
    return int.from_bytes(b[:n], 'little') if b else None


def wait_parked(c, max_s=10.0):
    """Poll the parked status until the game pauses or timeout."""
    deadline = time.time() + max_s
    while time.time() < deadline:
        r = c.cmd('parked')
        if r.get('parked'):
            return r
        time.sleep(0.05)
    return None


def main():
    launch()
    c = DebugClient(RECOMP_PORT)
    try:
        c.cmd('pause')
        # Get to f95 first.
        step_to(c, 95)
        # Arm a breakpoint at $00:$EBA5 (WallRun-call block in EB77).
        r = c.cmd('break_add eba5')
        print(f'break_add: {r}')
        # Resume execution. The game will pause when block_hook for $eba5 fires.
        c.cmd('continue')
        parked = wait_parked(c, 10.0)
        if parked is None:
            print('TIMEOUT — parked never set. Check that block ran.')
            return
        print(f'\nPARKED at: {parked}')
        # Read state at the parked moment.
        state = {
            'PlayerXPosNext': rb(c, 0x94, 2),
            'PlayerYPosNext': rb(c, 0x96, 2),
            'PlayerYPosNow':  rb(c, 0xD3, 2),
            'PlayerYSpeed':   rb(c, 0x7C, 2),
            'PlayerInAir':    rb(c, 0x72),
            'PlayerBlockedDir': rb(c, 0x77),
            'OnGround':       rb(c, 0x13EF, 2),
            'Powerup':        rb(c, 0x19),
            'PlayerIsDucking': rb(c, 0x73),
            'RidingYoshi':    rb(c, 0x187A, 2),
            '$90 (PlayerYPosInBlock)': rb(c, 0x90),
            '$91 (PlayerBlockMoveY)': rb(c, 0x91),
            '$92 (PlayerXPosInBlock)': rb(c, 0x92),
            '$93 (PlayerBlockXSide)': rb(c, 0x93),
            '$98 (TouchBlockYPos)': rb(c, 0x98, 2),
            '$9A (TouchBlockXPos)': rb(c, 0x9A, 2),
        }
        print(f'\nState at $eba5:')
        for k, v in state.items():
            print(f'  {k:32s} = 0x{v:04x}' if v is not None else f'  {k:32s} = ?')

        # Step to next block to verify Tier 2.5 step works.
        c.cmd('step_block')
        parked = wait_parked(c, 5.0)
        if parked:
            print(f'\nAfter step_block, parked at: {parked["pc"]}')
        # Cleanup: clear breakpoints, continue.
        c.cmd('break_clear')
        c.cmd('continue')
    finally:
        c.close(); _kill()


if __name__ == '__main__':
    main()
