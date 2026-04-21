"""Direct A/B comparison of oracle vs recomp at the specific WRAM
addresses iter 31's compute path reads.

We've verified on recomp:
  $7E:$CB17 = 0x25   (Map16LowPtr[Y=0x167])
  $7F:$CB17 = 0x00   (Map16HighPtr[Y=0x167])
  $7E:$1008 = $8128  (Map16Pointers[0x25*2=0x4A])
And $0D:$8128 in ROM contains 0x10F8 (blank tile).

If oracle has DIFFERENT values at any of these, the input-state theory
(both sides process iter 31 identically) is falsified and we have
specific state divergence to investigate.
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
        for target in [94, 95, 96]:
            step_to(r, target); step_to(o, target)
            print(f'\n=== f{target} ===')
            addrs = [
                (0x0CB17, 2, 'Map16LowPtr[0x167] (byte used in tile_idx low)'),
                (0x1CB17, 2, 'Map16HighPtr[0x167] (byte used in tile_idx high)'),
                (0x01008, 2, 'Map16Pointers[0x25] (the 16-bit ROM addr)'),
                (0x00045, 4, 'Layer1TileUp/Down ($45+)'),
                (0x00055, 1, 'Layer1ScrollDir'),
                (0x01925, 1, 'LevelModeSetting'),
                (0x01928, 1, 'LevelLoadObject (outer loop counter)'),
                (0x01931, 1, 'ObjectTileset'),
                (0x01BE4, 2, 'Layer1VramAddr'),
                (0x01C3E, 2, 'Layer1VramBuffer[$58] — the disputed cell'),
            ]
            print(f'  {"addr":<8} {"size":<4} {"recomp":<20} {"oracle":<20} m  label')
            for a, n, label in addrs:
                rv = rb(r, a, n); ov = rb(o, a, n)
                rh = rv.hex() if rv else ''
                oh = ov.hex() if ov else ''
                m = 'Y' if rv == ov else 'DIFF'
                print(f'  0x{a:05x}  {n}    {rh:<20} {oh:<20} {m}  {label}')
    finally:
        r.close(); o.close(); _kill()


if __name__ == '__main__':
    main()
