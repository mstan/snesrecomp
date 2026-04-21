"""Capture recomp's block-level execution trace inside RunPlayerBlockCode_EB77
during f95. Compare the sequence of label_xxxx visits against SMWDisX
expected branch path. The divergence point identifies which conditional
in EB77 takes the wrong branch in recomp.

EB77 lives at $00:$EB77 → $00:$EE5C-ish. Filter by func name.
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
        c.cmd('trace_blocks_reset')
        c.cmd('trace_blocks')
        step_to(c, 96)
        # Filter to blocks whose func contains EB77 (the function we're
        # debugging) — that gives us the exact path.
        # Get blocks INSIDE EB77 + the WallRun helper it calls.
        # Filter by func substring "EB77" — any block hook recorded while
        # g_last_recomp_func contains EB77 (catches both the fn itself and
        # any stack-restore quirks).
        trace = c.cmd('get_block_trace from=95 to=95 func=EB77')
        log = trace.get('log', [])
        print(f'Captured {trace.get("emitted", 0)} blocks in EB77 at f95 (total ring: {trace.get("entries", 0)})\n')
        for e in log:
            print(f'  pc={e["pc"]} d={e["d"]} func={e["func"]}')
    finally:
        c.close(); _kill()


if __name__ == '__main__':
    main()
