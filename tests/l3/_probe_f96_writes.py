"""At frame 95, trace VRAM writes to $V28D0-$V2940 (the differing region)
during frame 96 execution. Show first divergent write with call stack.
"""
import sys, pathlib, time, subprocess, socket
THIS_DIR = pathlib.Path(__file__).parent
sys.path.insert(0, str(THIS_DIR))
from harness import RECOMP_EXE, ORACLE_EXE, RECOMP_PORT, ORACLE_PORT, DebugClient  # noqa: E402


def _kill():
    subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'], capture_output=True)


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
        step_to(r, 95); step_to(o, 95)
        # trace writes to the differing region only
        r.cmd('trace_vram_reset'); o.cmd('trace_vram_reset')
        r.cmd('trace_vram 28d0 2940'); o.cmd('trace_vram 28d0 2940')
        step_to(r, 96); step_to(o, 96)
        rt = r.cmd('get_vram_trace')  # WITH stack for recomp to see call chain
        ot = o.cmd('get_vram_trace nostack')
        rlog = rt.get('log', [])
        olog = ot.get('log', [])
        print(f'[trace] recomp={len(rlog)} oracle={len(olog)} writes to $V28d0-$V2940')
        # print side-by-side
        print('\n=== RECOMP first 30 writes ===')
        for i, e in enumerate(rlog[:30]):
            stk = e.get('stack', [])
            top = stk[-1] if stk else ''
            print(f'  [{i:3d}] f{e["f"]} ${e["adr"]}={e["val"]} top={top}')
        print('\n=== ORACLE first 30 writes ===')
        for i, e in enumerate(olog[:30]):
            print(f'  [{i:3d}] f{e["f"]} ${e["adr"]}={e["val"]}')
        # Find first temporal divergence
        print('\n=== FIRST DIFFERING WRITE ===')
        for i in range(max(len(rlog), len(olog))):
            ri = rlog[i] if i < len(rlog) else None
            oi = olog[i] if i < len(olog) else None
            if ri is None or oi is None or ri['adr'] != oi['adr'] or ri['val'] != oi['val']:
                print(f'at idx {i}:')
                print(f'  recomp: {ri}')
                print(f'  oracle: {oi}')
                if ri and ri.get('stack'):
                    print(f'  recomp call stack:')
                    for s in ri['stack']: print(f'    {s}')
                break
    finally:
        r.close(); o.close(); _kill()


if __name__ == '__main__':
    main()
