"""At f95 (paused), invoke BufferScrollingTiles_Layer1 directly on both
sides. Compare Layer1VramBuffer output. If recomp and oracle diverge
at known-same state, the generated function is buggy.
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


def dump_l1vb(c, label):
    # Layer1VramBuffer is at $7E:1BE6, 256 bytes
    b = rb(c, 0x1BE6, 256)
    print(f'=== {label} Layer1VramBuffer ===')
    for i in range(0, len(b), 16):
        print(f'  ${0x1BE6+i:04x}: {" ".join(f"{x:02x}" for x in b[i:i+16])}')
    return b


def main():
    launch_both()
    r = DebugClient(RECOMP_PORT); o = DebugClient(ORACLE_PORT)
    try:
        r.cmd('pause'); o.cmd('pause')
        target = int(sys.argv[1]) if len(sys.argv) > 1 else 95
        step_to(r, target); step_to(o, target)
        # At f95 both sides are paused with identical input state. Invoke.
        print('[pre-invoke] comparing Layer1VramBuffer...')
        rb_pre = rb(r, 0x1BE6, 256)
        ob_pre = rb(o, 0x1BE6, 256)
        if rb_pre != ob_pre:
            print('  PRE-INVOKE BUFFERS DIFFER — proceeding anyway')
            for i, (a, b) in enumerate(zip(rb_pre, ob_pre)):
                if a != b:
                    print(f'  first diff at offset {i:04x} ($1BE6+{i:04x}) R=0x{a:02x} O=0x{b:02x}')
                    break
        else:
            print('  pre-invoke: MATCH')

        # Invoke
        print('[invoke] BufferScrollingTiles_Layer1 on recomp and oracle...')
        rr = r.cmd('invoke_recomp BufferScrollingTiles_Layer1')
        oo = o.cmd('invoke_recomp BufferScrollingTiles_Layer1')
        print(f'  recomp response: {rr}')
        print(f'  oracle response: {oo}')

        # Compare
        rb_post = rb(r, 0x1BE6, 256)
        ob_post = rb(o, 0x1BE6, 256)
        print('[post-invoke] comparing...')
        if rb_post == ob_post:
            print('  MATCH — generated function output is identical')
        else:
            diffs = [(i, rb_post[i], ob_post[i]) for i in range(256) if rb_post[i] != ob_post[i]]
            print(f'  DIFFER — {len(diffs)} bytes differ')
            for off, r_b, o_b in diffs[:30]:
                print(f'    $1BE6+0x{off:02x} (${0x1BE6+off:04x}): R=0x{r_b:02x} O=0x{o_b:02x}')
            if len(diffs) > 30:
                print(f'    ... {len(diffs)-30} more')

        # Also show the last 16 bytes on both
        print('\n[post-invoke] full buffers:')
        dump_l1vb(r, 'RECOMP')
        dump_l1vb(o, 'ORACLE')
    finally:
        r.close(); o.close(); _kill()


if __name__ == '__main__':
    main()
