"""Bug #8 phase 11: diff Mario-state WRAM between recomp and emu
at the frame where recomp first fires the wrong-path $72 write
(recomp f216, writing $72=0x0B via HandlePlayerPhysics).

The first Mario-state byte that diverges BEFORE f216 is the upstream
cause. Walks a small set of Mario-physics addresses plus the broader
zero-page and $13xx-$14xx ranges.

Strategy:
  1. Put recomp at f215 (one frame before the divergent write).
  2. Put emu at its equivalent game-state (sync by GameMode=07,
     the title-screen demo mode, OR by matching some invariant).
  3. Dump Mario-state WRAM on both sides.
  4. Report the diffs; highlight the "physics-critical" bytes that
     would steer HandlePlayerPhysics to a different branch.
"""
import sys, pathlib, time, subprocess, socket
THIS_DIR = pathlib.Path(__file__).parent
sys.path.insert(0, str(THIS_DIR))
from harness import DebugClient, RECOMP_PORT  # noqa: E402

REPO = pathlib.Path(__file__).parent.parent.parent.parent
ORACLE_EXE = REPO / 'build' / 'bin-x64-Oracle' / 'smw.exe'

# Mario physics / state-critical bytes from SMWDisX symbol table.
# Any divergence here could steer HandlePlayerPhysics to a different
# branch. Grouped by subsystem.
MARIO_STATE = [
    # Player position / velocity
    ('PlayerXPosNext',   0x0094, 2),
    ('PlayerYPosNext',   0x0096, 2),
    ('PlayerXPosNow',    0x00D1, 2),
    ('PlayerYPosNow',    0x00D3, 2),
    ('PlayerXSpeed',     0x007B, 2),
    ('PlayerYSpeed',     0x007D, 2),
    # Player physics flags / pose
    ('PlayerInAir',      0x0072, 1),
    ('PlayerIsDucking',  0x0073, 1),
    ('PlayerOnGround',   0x13EF, 1),
    ('PlayerBlockedDir', 0x0077, 1),
    ('PlayerIsClimbing', 0x0074, 1),
    ('PlayerSlopePose',  0x008A, 1),
    ('PlayerAnimation',  0x0071, 1),
    ('PlayerDirection',  0x0076, 1),
    # Touched-block info (collision)
    ('Map16TileNumber',  0x009C, 1),
    ('TouchBlockYPos',   0x0098, 2),
    ('TouchBlockXPos',   0x009A, 2),
    ('PlayerBlockMoveY', 0x0091, 1),
    # In-air / flight state
    ('MaxStageOfFlight', 0x149F, 1),
    ('FlightPhase',      0x1407, 1),
    ('TempPlayerGround', 0x18C8, 1),
    ('TempPlayerAir',    0x18C9, 1),
    # Input / timers
    ('byetudlrFrame',    0x0015, 1),
    ('byetudlrHold',     0x0016, 1),
    ('axlr0000Frame',    0x0017, 1),
    ('axlr0000Hold',     0x0018, 1),
    ('PipeTimer',        0x1403, 1),
    ('TakeoffTimer',     0x149F, 1),
    # Cape/flight
    ('CapeTailSpin',     0x14AD, 1),
    # Game mode / frame
    ('GameMode',         0x0100, 1),
    ('EffFrame',         0x0013, 1),
    # Yoshi / misc state
    ('PlayerRidingYoshi',0x187A, 2),
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


def rb(c, cmd, addr, w=1):
    r = c.cmd(f'{cmd} 0x{addr:x} {w}')
    h = r.get('hex', '').replace(' ', '')
    if not h: return None
    return int.from_bytes(bytes.fromhex(h)[:w], 'little') if h else None


def step_to(c, target):
    cur = c.cmd('frame').get('frame', 0)
    if cur >= target: return cur
    c.cmd(f'step {target - cur}')
    deadline = time.time() + 60
    while time.time() < deadline:
        if c.cmd('frame').get('frame', 0) >= target: return target
        time.sleep(0.03)


def dump(c, cmd):
    out = {}
    for label, addr, w in MARIO_STATE:
        out[label] = (addr, w, rb(c, cmd, addr, w))
    return out


def fmt(v, w):
    if v is None: return '??' * w
    return f'0x{v:0{w*2}x}'


def find_equivalent_emu_frame(c):
    """Step emu forward in small chunks until its GameMode matches
    recomp's current GameMode. Prior probes: GameMode=07 (title demo)
    is where Mario-physics code runs. Return total emu-extra frames."""
    # Recomp at f215 is in GameMode=07 (title-screen demo).
    # From phase-1/2: emu reaches GameMode=07 around emu-frame ~330.
    # step_to(c, 215) already advanced emu 215 frames via main-loop tick.
    # Walk emu up in chunks of 30 until GameMode matches recomp.
    target_mode = rb(c, 'read_ram', 0x100, 1)
    print(f'Recomp GameMode=0x{target_mode:02x}')
    extra = 0
    for _ in range(30):
        em = rb(c, 'emu_read_wram', 0x100, 1)
        if em == target_mode:
            return extra
        c.cmd('emu_step 20')
        extra += 20
    return extra


def main():
    launch()
    c = DebugClient(RECOMP_PORT)
    try:
        c.cmd('pause')

        # Step recomp to f215 (the frame BEFORE the divergent $72 write
        # at f216). This is the last frame both sides should still be in
        # a comparable state — anything that differs here is upstream.
        step_to(c, 215)
        print(f'Recomp at f{c.cmd("frame").get("frame")} '
              f'GameMode=0x{rb(c,"read_ram",0x100):02x} '
              f'$72=0x{rb(c,"read_ram",0x72):02x}')

        # Sync emu to same GameMode.
        extra = find_equivalent_emu_frame(c)
        print(f'Synced emu with +{extra} extra frames. '
              f'emu GameMode=0x{rb(c,"emu_read_wram",0x100):02x} '
              f'emu $72=0x{rb(c,"emu_read_wram",0x72):02x}')
        print()

        rec = dump(c, 'read_ram')
        emu = dump(c, 'emu_read_wram')

        print(f'{"variable":<22}  {"addr":>6} {"width":>5}   {"recomp":>8}   {"emu":>8}   diff')
        print('-' * 72)
        diverged = []
        for label, addr, w in MARIO_STATE:
            _, _, rv = rec[label]
            _, _, ev = emu[label]
            mark = '  <-- DIFF' if rv != ev else ''
            if rv != ev: diverged.append(label)
            print(f'{label:<22}  0x{addr:04x} {w:5}   {fmt(rv,w):>8}   {fmt(ev,w):>8}{mark}')

        print()
        print(f'Diverged: {len(diverged)} / {len(MARIO_STATE)}')
        if diverged:
            print('  ' + ', '.join(diverged))

        # Per-byte diff of raw zero-page too — cheaper overview.
        print()
        print('Raw ZP diff $00-$FF:')
        rec_zp = bytes.fromhex(c.cmd('read_ram 0 256').get('hex','').replace(' ',''))
        emu_zp = bytes.fromhex(c.cmd('emu_read_wram 0 256').get('hex',''))
        zp_diffs = [(i, rec_zp[i], emu_zp[i]) for i in range(min(len(rec_zp),len(emu_zp)))
                    if rec_zp[i] != emu_zp[i]]
        print(f'  {len(zp_diffs)} bytes differ')
        for addr, rv, ev in zp_diffs[:40]:
            print(f'    $00{addr:02x}: recomp=0x{rv:02x}  emu=0x{ev:02x}')

        c.cmd('continue')
    finally:
        c.close(); _kill()


if __name__ == '__main__':
    main()
