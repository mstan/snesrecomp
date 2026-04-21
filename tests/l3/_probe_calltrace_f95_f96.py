"""Capture recomp's full RecompStackPush call sequence during f95-f96
(when oracle clears PlayerInAir but recomp doesn't). Walk the sequence
to see which functions were entered in RunPlayerBlockCode's call chain
on the way (or not) to $EF60.

Useful filters:
  - Only entries with parent matching a target chain (HandlePlayerLevelCollision,
    RunPlayerBlockCode_*, RunPlayerBlockCode_EB77, etc.)
  - Print at depths 4-12 to keep noise down (frame loop is depth ~3).
"""
import sys, pathlib, time, subprocess, socket
THIS_DIR = pathlib.Path(__file__).parent
sys.path.insert(0, str(THIS_DIR))
from harness import RECOMP_EXE, RECOMP_PORT, DebugClient  # noqa: E402


def _kill(): subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'], capture_output=True)


def launch():
    _kill(); time.sleep(0.5)
    subprocess.Popen([str(RECOMP_EXE), '--paused'],
        cwd=str(RECOMP_EXE.parent.parent.parent),
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    deadline = time.time() + 15
    while time.time() < deadline:
        try:
            s = socket.create_connection(('127.0.0.1', RECOMP_PORT), timeout=0.3); s.close(); return
        except OSError: time.sleep(0.2)
    raise RuntimeError('no connect')


def step_to(c, target):
    base = c.cmd('frame').get('frame', 0)
    if base >= target: return base
    c.cmd(f'step {target - base}')
    deadline = time.time() + 60
    while time.time() < deadline:
        if c.cmd('frame').get('frame', 0) >= target: return target
        time.sleep(0.05)


def main():
    launch()
    c = DebugClient(RECOMP_PORT)
    try:
        c.cmd('pause')
        step_to(c, 95)
        c.cmd('trace_calls_reset')
        c.cmd('trace_calls')
        step_to(c, 97)
        # First: hunt for any push containing "Player" in f95-f96.
        trace = c.cmd('get_call_trace from=95 to=96 contains=Player')
        log = trace.get('log', [])
        print(f'Captured {trace.get("emitted", 0)} entries f95-f96 (total ring: {trace.get("entries", 0)})\n')
        # Filter to relevant chain.
        TARGETS = {
            'HandlePlayerLevelCollision', 'RunPlayerBlockCode',
            'RunPlayerBlockCode_EB77', 'RunPlayerBlockCode_00EB48',
            'RunPlayerBlockCode_EB76', 'RunPlayerBlockCode_00EB73',
            'GetPlayerLevelCollisionMap16ID_WallRun',
            'PlayerState00_00CD24', 'PlayerState00_00CD36',
            'HandlePlayerLevelColl_00E98C',
            'PlayerState00_00CCE0', 'PlayerState00',
            'HandlePlayerPhysics_D930',
        }
        # Print all entries during f95-f96 with depth indent.
        for e in log:
            f = e['f']; d = e['d']; fn = e['func']; pa = e['parent']
            indent = '  ' * min(d, 12)
            tag = ''
            if fn in TARGETS:
                tag = '  **'
            elif any(s in fn for s in ('PlayerBlockCode', 'PlayerLevelColl', 'PlayerState00')):
                tag = '  *'
            print(f'f{f} d{d:>2}{indent}{fn}  (from{pa}){tag}')
    finally:
        c.close(); _kill()


if __name__ == '__main__':
    main()
