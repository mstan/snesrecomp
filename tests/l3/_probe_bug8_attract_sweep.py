"""Bug #8 investigation, phase 1: attract-demo WRAM sweep.

Drives the Oracle build (recomp + embedded snes9x) through the first
~400 attract-demo frames and logs the first divergence on each
Mario-state WRAM byte. Confirms whether bug #8 is reachable from ROM
boot or only surfaces after a save-state load.

Output format per frame:
  f N:  recomp[72 94 96 13EF 19 72]  vs  emu[...]   (first diff only)

Termination: first frame where ANY tracked byte differs, OR max frame.

Usage from repo root:
    python snesrecomp/tests/l3/_probe_bug8_attract_sweep.py
"""
import sys, pathlib, time, subprocess, socket
THIS_DIR = pathlib.Path(__file__).parent
sys.path.insert(0, str(THIS_DIR))
from harness import DebugClient, RECOMP_PORT  # noqa: E402

REPO = pathlib.Path(__file__).parent.parent.parent.parent
ORACLE_EXE = REPO / 'build' / 'bin-x64-Oracle' / 'smw.exe'

# Each: (label, addr, width_bytes)
WATCH = [
    ('PlayerInAir',     0x72,   1),
    ('PlayerBlockedDir',0x77,   1),
    ('PlayerYSpeed',    0x7C,   2),
    ('PlayerXPosNext',  0x94,   2),
    ('PlayerYPosNext',  0x96,   2),
    ('PlayerYPosNow',   0xD3,   2),
    ('OnGround',        0x13EF, 1),
    ('Powerup',         0x19,   1),
    ('RidingYoshi',     0x187A, 2),
    ('GameMode',        0x100,  1),   # SMW game-mode byte
    ('NmiFrame',        0x13,   1),   # lo byte of NMI frame counter
]

MAX_FRAME = 400


def _kill():
    subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'], capture_output=True)


def launch():
    if not ORACLE_EXE.exists():
        raise RuntimeError(f'Oracle build missing: {ORACLE_EXE}')
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


def rb(c, cmd, addr, width):
    r = c.cmd(f'{cmd} 0x{addr:x} {width}')
    if not r.get('ok') and cmd != 'read_ram':
        return None
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


def fmt(val, width):
    if val is None:
        return '??' * width
    return f'{val:0{width*2}x}'


def main():
    launch()
    c = DebugClient(RECOMP_PORT)
    first_diff = {label: None for label, _, _ in WATCH}
    try:
        c.cmd('pause')

        # Pre-flight.
        el = c.cmd('emu_list')
        print(f'emu_list: active={el.get("active")!r}  backends={el.get("backends")}')
        if el.get('active') != 'snes9x':
            print('ABORT: snes9x not active'); return

        print()
        print('frame |                                            recomp                                            | '
              '                                            emu                                               | diff?')
        print('------+------------------------------------------------------------------------------------------------+------')

        # Sample at a small set of checkpoints first to keep output sane,
        # then zoom in on whichever watch fires first.
        checkpoints = [10, 30, 60, 90, 94, 100, 120, 150, 200, 250, 300, 400]
        for target in checkpoints:
            if target > MAX_FRAME: break
            step_to(c, target)
            f = c.cmd('frame').get('frame', 0)

            rec_line = []
            emu_line = []
            any_diff = False
            for label, addr, width in WATCH:
                rv = rb(c, 'read_ram', addr, width)
                ev = rb(c, 'emu_read_wram', addr, width)
                rs = fmt(rv, width)
                es = fmt(ev, width)
                diff = (rv != ev)
                marker = '!' if diff else ' '
                rec_line.append(f'{label[:4]}={rs}{marker}')
                emu_line.append(f'{label[:4]}={es}{marker}')
                if diff:
                    any_diff = True
                    if first_diff[label] is None:
                        first_diff[label] = (f, rv, ev)

            print(f' {f:4d} | {"  ".join(rec_line):<94} | {"  ".join(emu_line):<94} | {"YES" if any_diff else " no"}')

        print()
        print('First-divergence summary:')
        for label, _, width in WATCH:
            if first_diff[label]:
                fr, rv, ev = first_diff[label]
                print(f'  {label:18s} diverged at f{fr:4d}: recomp=0x{fmt(rv,width)}  emu=0x{fmt(ev,width)}')
            else:
                print(f'  {label:18s} matched through f{MAX_FRAME}')

        # If $72 ever diverged, also dump emu CPU regs at that frame for
        # immediate PC attribution.
        if first_diff['PlayerInAir']:
            fr, _, _ = first_diff['PlayerInAir']
            print(f'\n$72 diverged at f{fr}. emu CPU state at that frame:')
            # (Game has already progressed past fr; this is a post-diff snapshot.)
            r = c.cmd('emu_cpu_regs')
            print(f'  {r}')

        c.cmd('continue')
    finally:
        c.close(); _kill()


if __name__ == '__main__':
    main()
