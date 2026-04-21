"""Check LevelModeSetting ($1925) and Layer1TileUp state across frames to
see what dispatch target BufferScrollingTiles_Layer1_Init selects on each side.
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
        f = c.cmd('frame').get('frame', 0)
        if f >= target: return f
        time.sleep(0.05)
    return c.cmd('frame').get('frame', 0)


def rb(c, addr, n):
    r = c.cmd(f'read_ram 0x{addr:x} {n}')
    return bytes.fromhex(r.get('hex', '').replace(' ', ''))


def main():
    launch_both()
    r = DebugClient(RECOMP_PORT); o = DebugClient(ORACLE_PORT)
    try:
        r.cmd('pause'); o.cmd('pause')
        for target in [94, 95, 96]:
            step_to(r, target); step_to(o, target)
            ram_addrs = [
                (0x0100, 1, 'GameMode ($7E:0100)'),
                (0x13C6, 1, 'GameMode secondary'),
                (0x13D9, 1, 'OWProcess'),
                (0x1925, 1, 'LevelModeSetting'),
                (0x13D7, 1, 'Layer1ScrollDir'),
                (0x45, 32, 'Layer1TileUp table (start)'),
                (0x1BE4, 2, 'Layer1VramAddr'),
                (0x1CE6, 2, 'Layer2VramAddr'),
                (0x55, 1, 'Level/misc'),
                (0x1931, 1, 'ObjectTileset'),
                (0x0FBE, 64, 'Map16Pointers (0..32)'),
                (0x1224, 64, 'Map16Pointers @ +$266 (outer loop writes here)'),
                (0x13B0, 16, 'Map16Pointers tail'),
                (0x6B, 6, '_6B..6F (Map16LowPtr+HighPtr)'),
                (0x8, 2, '_8 (BufferScrollingTiles loop counter)'),
                (0x0, 4, '_0.._3 temp'),
                (0xa, 4, '_A.._D ptr'),
                (0x1BE6, 16, 'L1VramBuffer first 16 bytes'),
                (0x1C26, 16, 'L1VramBuffer +0x40 (diff region) first 16'),
                (0x1C66, 16, 'L1VramBuffer +0x80 first 16'),
            ]
            print(f'\n=== f{target} ===')
            for a, n, name in ram_addrs:
                rv = rb(r, a, n); ov = rb(o, a, n)
                rhex = rv.hex()
                ohex = ov.hex()
                m = 'MATCH' if rv == ov else 'DIFF '
                print(f'  ${a:04x}+{n}: R={rhex} O={ohex} {m}  ({name})')
    finally:
        r.close(); o.close(); _kill()


if __name__ == '__main__':
    main()
