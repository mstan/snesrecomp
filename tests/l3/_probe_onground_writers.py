"""Trace WRAM writes to PlayerIsOnGround ($13EF) f0->f200 on both
sides. We expect oracle to write 1 to it during level-load (boot or
attract-demo entry), and recomp to NOT write it (or write 0 instead).
The function that writes it is the missing/broken init.
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


def main():
    launch_both()
    r = DebugClient(RECOMP_PORT); o = DebugClient(ORACLE_PORT)
    try:
        r.cmd('pause'); o.cmd('pause')
        step_to(r, 0); step_to(o, 0)
        # Recomp Tier 1 trace.
        r.cmd('trace_wram_reset')
        r.cmd('trace_wram 72 72')
        r.cmd('trace_wram 77 77')
        r.cmd('trace_wram 13ef 13ef')
        step_to(r, 200)
        trace = r.cmd('get_wram_trace')
        log = trace.get('log', [])
        print(f'=== Recomp writes to $13EF f0-f200 ({len(log)} writes) ===')
        for e in log[:80]:
            a = int(e['adr'], 16)
            label = {0x72: 'PlayerInAir', 0x77: 'PlayerBlockedDir', 0x13ef: 'OnGround'}.get(a, '?')
            print(f'  f{e["f"]} ${a:04x}({label}) val=0x{int(e["val"], 16):x} w={e["w"]} fn={e["func"]} parent={e.get("parent", "?")}')
        # Note: oracle has no Tier 1, but we know it sets to 1 by f195.
        print(f'\n[oracle] sets $13EF=1 by f195 (confirmed via probe).')
    finally:
        r.close(); o.close(); _kill()


if __name__ == '__main__':
    main()
