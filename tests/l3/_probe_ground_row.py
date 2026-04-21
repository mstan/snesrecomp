"""Compare Map16Lo at Mario's column on recomp vs oracle at f250
(after level-load, before Mario lands). Find which row holds the ground
tile in each — if oracle has ground at row 22 and recomp has it at
row 23, that's the placer bug; if both have it at row 22, the bug is
in the collision-detection path.

Mario's column at f250 in oracle ≈ X=168/16 = 10. Map16Lo for column 10
starts at $7E:$BB50 (= $BAB0 + 10*16). Dump all 16 rows on both sides
and find where they diverge.
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
        # Step both to the same frame.
        F = 250
        step_to(r, F); step_to(o, F)
        # Dump Map16Lo full sweep $7E:$C800-$D800 (level data area).
        # Find every byte that differs between sides.
        print(f'=== Map16Lo $7E:$C800-$D800 byte-diff at f{F} ===')
        rb_data = rb(r, 0xC800, 0x1000)
        ob_data = rb(o, 0xC800, 0x1000)
        diffs = 0
        for i in range(min(len(rb_data), len(ob_data))):
            if rb_data[i] != ob_data[i]:
                if diffs < 60:
                    print(f'  ${0xC800 + i:04x}  recomp=0x{rb_data[i]:02x}  oracle=0x{ob_data[i]:02x}')
                diffs += 1
        print(f'Total diffs: {diffs}')
        # Also dump bank-7F Map16High (collision attribute byte) same range.
        print(f'\n=== Map16Hi $7F:$C800-$D800 byte-diff at f{F} ===')
        rb_data = rb(r, 0x1C800, 0x1000)
        ob_data = rb(o, 0x1C800, 0x1000)
        diffs = 0
        for i in range(min(len(rb_data), len(ob_data))):
            if rb_data[i] != ob_data[i]:
                if diffs < 60:
                    print(f'  ${0x1C800 + i:05x}  recomp=0x{rb_data[i]:02x}  oracle=0x{ob_data[i]:02x}')
                diffs += 1
        print(f'Total diffs: {diffs}')
    finally:
        r.close(); o.close(); _kill()


if __name__ == '__main__':
    main()
