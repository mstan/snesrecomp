"""Trace ALL writes in $70-$8F (player physics zone) in recomp f95-f105.
This shows the full function call sequence touching player state, so we
can compare against SMWDisX and identify which oracle-side function is
missing.
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
        c.cmd('trace_wram_reset')
        c.cmd('trace_wram 71 7f')   # skip $70 (BufferScrollingTiles noise)
        c.cmd('trace_wram 13ef 13ef')
        step_to(c, 105)

        trace = c.cmd('get_wram_trace')
        log = trace.get('log', [])
        print(f'Captured {trace.get("entries", 0)} writes f95-f105:\n')
        # Group by frame.
        by_frame = {}
        for e in log:
            by_frame.setdefault(e['f'], []).append(e)
        for f in sorted(by_frame.keys()):
            print(f'--- f{f} ---')
            for e in by_frame[f]:
                a = int(e['adr'], 16)
                print(f'  ${a:04x} val=0x{int(e["val"], 16):02x} w={e["w"]} fn={e["func"]} parent={e.get("parent", "?")}')
    finally:
        c.close(); _kill()


if __name__ == '__main__':
    main()
