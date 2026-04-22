"""Bug #8 phase 19: diff every WRAM byte that PlayerState00 reads
to decide whether to enter the HandlePlayerPhysics chain (which
contains the HPD930 path that's spuriously firing on recomp).

We capture both sides at the equivalent game-state moment (f200 on
recomp, just before accumulation begins, vs synced emu frame). The
first byte that differs is the upstream cause of the divergent
branch.
"""
import sys, pathlib, time, subprocess, socket
THIS_DIR = pathlib.Path(__file__).parent
sys.path.insert(0, str(THIS_DIR))
from harness import DebugClient, RECOMP_PORT  # noqa: E402

REPO = pathlib.Path(__file__).parent.parent.parent.parent
ORACLE_EXE = REPO / 'build' / 'bin-x64-Oracle' / 'smw.exe'

# Every byte read by CODE_00CD24 / CODE_00CD39 / CODE_00CD79 path
# (per SMWDisX bank_00.asm 8897-8949) that gates the call into
# HandlePlayerPhysics ($D5F2) / HandlePlayerPhysics_InAir ($D7E4).
INPUTS = [
    ('PlayerInAir',           0x0072, 1),
    ('PlayerIsClimbing',      0x0074, 1),
    ('PlayerInWater',         0x0075, 1),
    ('PlayerBlockedDir',      0x0077, 1),
    ('PlayerYSpeed+1',        0x007D, 1),
    ('InteractionPtsClimbable',0x008B, 1),
    ('GameMode',              0x0100, 1),
    ('PBalloonInflating',     0x13F3, 1),
    ('IsCarryingItem',        0x148F, 1),
    ('PlayerRidingYoshi',     0x187A, 2),
    ('PlayerClimbingRope',    0x18BE, 1),
    # Input bytes that affect the climbing branch
    ('byetudlrHold',          0x0016, 1),
    ('byetudlrFrame',         0x0015, 1),
]


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


def rb(c, cmd, addr, w=1):
    r = c.cmd(f'{cmd} 0x{addr:x} {w}')
    h = r.get('hex','').replace(' ','')
    if not h: return None
    return int.from_bytes(bytes.fromhex(h)[:w],'little')


def main():
    launch()
    c = DebugClient(RECOMP_PORT)
    try:
        c.cmd('pause')
        # Step recomp to f200 (right BEFORE the accumulation starts at f201).
        step_to(c, 200)
        # Sync emu to GameMode=07.
        target_mode = rb(c, 'read_ram', 0x100)
        for _ in range(40):
            if rb(c, 'emu_read_wram', 0x100) == target_mode: break
            c.cmd('emu_step 20')
        print(f'Recomp at f200 GameMode=0x{target_mode:02x}')
        print(f'Emu synced (GameMode=0x{rb(c,"emu_read_wram",0x100):02x})')
        print()

        # Diff every gating input.
        print(f'{"variable":<26}  {"recomp":>8}   {"emu":>8}   diff')
        print('-' * 60)
        diverged = []
        for label, addr, w in INPUTS:
            rv = rb(c, 'read_ram', addr, w)
            ev = rb(c, 'emu_read_wram', addr, w)
            mark = '  <-- DIFF' if rv != ev else ''
            if rv != ev: diverged.append((label, addr, rv, ev))
            fmt_r = ('??' if rv is None else f'0x{rv:0{w*2}x}')
            fmt_e = ('??' if ev is None else f'0x{ev:0{w*2}x}')
            print(f'{label:<26}  {fmt_r:>8}   {fmt_e:>8}{mark}')

        print()
        print(f'Diverged: {len(diverged)} / {len(INPUTS)}')
        if diverged:
            print('First-divergence candidates:')
            for label, addr, rv, ev in diverged:
                print(f'  $0{addr:04x} {label}: recomp=0x{rv:x}, emu=0x{ev:x}')

        # SMWDisX semantic: the path leading to HPD930 (CODE_00CD24)
        # requires the BPL at $CD24 to NOT branch (i.e. PlayerYSpeed+1
        # has high bit set / negative) AND PlayerBlockedDir & 0x08 == 0.
        # Or to take the BPL branch (PYS+1 positive) and continue.
        # Check that branch:
        rec_pys = rb(c, 'read_ram', 0x7D)
        emu_pys = rb(c, 'emu_read_wram', 0x7D)
        rec_bpl_taken = (rec_pys is not None and rec_pys < 0x80)
        emu_bpl_taken = (emu_pys is not None and emu_pys < 0x80)
        print()
        print(f'$CD24 BPL (PlayerYSpeed+1 >= 0):  recomp={rec_bpl_taken}  emu={emu_bpl_taken}')

        c.cmd('continue')
    finally:
        c.close(); _kill()


if __name__ == '__main__':
    main()
