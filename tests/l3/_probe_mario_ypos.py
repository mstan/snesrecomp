"""Compare recomp vs oracle PlayerYPos/XPos across attract-demo frames
to decide: is Mario physically one block lower (position bug) or is the
BG1 tilemap one row higher (rendering bug)?

  $96 = PlayerYPosNext (word)
  $D3 = PlayerYPosNow  (word)
  $94 = PlayerXPosNext (word)
  $D1 = PlayerXPosNow  (word)

Run recomp + oracle paused, step both to the same frame, dump.
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


def rw(c, addr):
    """Read a word from WRAM (little-endian)."""
    r = c.cmd(f'read_ram 0x{addr:x} 2')
    b = bytes.fromhex(r.get('hex', '').replace(' ', ''))
    if len(b) < 2: return None
    return b[0] | (b[1] << 8)


def main():
    launch_both()
    r = DebugClient(RECOMP_PORT); o = DebugClient(ORACLE_PORT)
    try:
        r.cmd('pause'); o.cmd('pause')
        frames = [50, 80, 100, 120, 140, 160, 175, 185, 190, 192, 194, 195, 200]
        print('frame  side     Ynow   Ynext  Yspd  OnGr')
        for f in frames:
            step_to(r, f); step_to(o, f)
            rY = rw(r, 0xD3); rYn = rw(r, 0x96); rYsp = rw(r, 0x7d); rOG = rw(r, 0x13ef)
            oY = rw(o, 0xD3); oYn = rw(o, 0x96); oYsp = rw(o, 0x7d); oOG = rw(o, 0x13ef)
            tag = ''
            if (rY or 0) != (oY or 0): tag += '  YDIF'
            if (rOG or 0) != (oOG or 0): tag += '  ONGR-DIF'
            print(f'f{f:<3} R {rY:>5} {rYn:>5} {rYsp:>5} {rOG:>5}{tag}')
            print(f'      O {oY:>5} {oYn:>5} {oYsp:>5} {oOG:>5}')
    finally:
        r.close(); o.close(); _kill()


if __name__ == '__main__':
    main()
