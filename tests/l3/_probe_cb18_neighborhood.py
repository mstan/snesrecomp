"""Compare recomp and oracle WRAM at $7E:$CB00-$CB40 at f96.
If recomp writes at $CB1F/$CB20 where oracle writes at $CB18, the
address-offset computation in HHSCCO (or its callee) differs between
faithful ROM interp and recomp's translation.
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
        step_to(r, 96); step_to(o, 96)
        rb_reg = rb(r, 0xCB00, 0x60)
        ob_reg = rb(o, 0xCB00, 0x60)
        print('addr      recomp  oracle  match')
        for i in range(0x60):
            a = 0xCB00 + i
            rv = rb_reg[i]
            ov = ob_reg[i]
            m = '=' if rv == ov else 'DIFF'
            if rv != 0x25 or ov != 0x25 or m != '=':
                # show only interesting cells (non-blank or diff)
                print(f'  ${a:04x}   0x{rv:02x}    0x{ov:02x}    {m}')
    finally:
        r.close(); o.close(); _kill()


if __name__ == '__main__':
    main()
