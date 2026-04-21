"""Watch $1928 (InitLevel outer loop counter) across frames 85-96 on both
sides. Compare per-frame value trajectories.
"""
import sys, pathlib, time, subprocess, socket
THIS_DIR = pathlib.Path(__file__).parent
sys.path.insert(0, str(THIS_DIR))
from harness import RECOMP_EXE, ORACLE_EXE, RECOMP_PORT, ORACLE_PORT, DebugClient  # noqa: E402


def _kill():
    subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'],
                   capture_output=True, check=False)


def _ports_ready():
    for port in (RECOMP_PORT, ORACLE_PORT):
        try:
            s = socket.create_connection(('127.0.0.1', port), timeout=0.3); s.close()
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
    raise RuntimeError('timeout')


def step_to(client, target):
    base = client.cmd('frame').get('frame', 0)
    if base >= target: return base
    client.cmd(f'step {target - base}')
    deadline = time.time() + 60
    while time.time() < deadline:
        f = client.cmd('frame').get('frame', 0)
        if f >= target: return f
        time.sleep(0.05)
    return client.cmd('frame').get('frame', 0)


def read_byte(c, adr):
    rv = c.cmd(f'read_ram 0x{adr:x} 1')
    hexs = rv.get('hex', '')
    return int(hexs.split()[0], 16) if hexs else None


def main():
    launch_both()
    r = DebugClient(RECOMP_PORT); o = DebugClient(ORACLE_PORT)
    try:
        r.cmd('pause'); o.cmd('pause')
        print(f'{"frame":<7} {"rec 1928":<10} {"orc 1928":<10} {"rec 1be4":<10} {"orc 1be4":<10}')
        for target in range(85, 101):
            step_to(r, target); step_to(o, target)
            r1928 = read_byte(r, 0x1928); o1928 = read_byte(o, 0x1928)
            r1be4 = read_byte(r, 0x1be4); o1be4 = read_byte(o, 0x1be4)
            m = '  <-- DIFF' if (r1928 != o1928 or r1be4 != o1be4) else ''
            print(f'{target:<7} 0x{r1928:02x}       0x{o1928:02x}       0x{r1be4:02x}       0x{o1be4:02x}{m}')
    finally:
        r.close(); o.close(); _kill()


if __name__ == '__main__':
    main()
