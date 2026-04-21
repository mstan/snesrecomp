"""Use trace_addr at $1C3E to see if ANY writes land there on recomp."""
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


def main():
    launch_both()
    r = DebugClient(RECOMP_PORT); o = DebugClient(ORACLE_PORT)
    try:
        r.cmd('pause'); o.cmd('pause')
        step_to(r, 94); step_to(o, 94)
        # trace_addr watches WRITES to a single byte
        # trace_addr only supports one addr at a time; run separate sessions per addr
        addr = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0x1c3e
        print(f'\n=== watching ${addr:04x} ===')
        for side, c in [('RECOMP', r), ('ORACLE', o)]:
            c.cmd(f'trace_addr {addr:x}')
        step_to(r, 97); step_to(o, 97)
        for side, c in [('RECOMP', r), ('ORACLE', o)]:
            tr = c.cmd('get_trace')
            log = tr.get('log', [])
            print(f'  [{side}] {len(log)} writes to ${addr:04x}:')
            for e in log[:8]:
                stk = e.get('stack', [])
                top = stk[-1] if stk else ''
                print(f'    f{e["f"]} 0x{e["old"]}->0x{e["new"]} func={e["func"]} top={top}')
            if len(log) > 8:
                print(f'    ... {len(log)-8} more')
    finally:
        r.close(); o.close(); _kill()


if __name__ == '__main__':
    main()
