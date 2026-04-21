"""Count total VRAM writes at f95 on recomp vs oracle across ALL VRAM."""
import sys, pathlib, time, subprocess, socket
THIS_DIR = pathlib.Path(__file__).parent
sys.path.insert(0, str(THIS_DIR))
from harness import RECOMP_EXE, ORACLE_EXE, RECOMP_PORT, ORACLE_PORT, DebugClient  # noqa: E402


def _kill():
    subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'],
                   capture_output=True, check=False)


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
        step_to(r, 80); step_to(o, 80)
        r.cmd('trace_vram_reset'); o.cmd('trace_vram_reset')
        r.cmd('trace_vram 0 7fff'); o.cmd('trace_vram 0 7fff')
        step_to(r, 100); step_to(o, 100)
        rt = r.cmd('get_vram_trace nostack')
        ot = o.cmd('get_vram_trace nostack')
        rlog = rt.get('log', []); olog = ot.get('log', [])
        print(f'recomp total VRAM writes: {rt.get("entries")} (log {len(rlog)})')
        print(f'oracle total VRAM writes: {ot.get("entries")} (log {len(olog)})')
        # Per frame
        from collections import Counter
        rf = Counter(e['f'] for e in rlog)
        of = Counter(e['f'] for e in olog)
        print('\nper-frame counts:')
        for f in sorted(set(rf) | set(of)):
            print(f'  f{f}: recomp={rf.get(f,0)} oracle={of.get(f,0)}')
        # Regions AT F95 only
        def region_bucket(a): return (a & 0x7f00)
        rr = Counter(region_bucket(int(e['adr'], 16)) for e in rlog if e['f']==95)
        oo = Counter(region_bucket(int(e['adr'], 16)) for e in olog if e['f']==95)
        print('\nper-256-word region @ f95:')
        for reg in sorted(set(rr) | set(oo)):
            print(f'  $V{reg:04x}-$V{reg+0xff:04x}: recomp={rr.get(reg,0)} oracle={oo.get(reg,0)}')
    finally:
        r.close(); o.close(); _kill()


if __name__ == '__main__':
    main()
