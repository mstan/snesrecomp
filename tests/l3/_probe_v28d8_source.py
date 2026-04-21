"""Trace what writes VRAM $V28D8 on both recomp and oracle across f80-f100.

Uses existing trace_vram (works on both sides — it's in both debug_servers,
not a Tier 1-only hook). This tells us which DMA VMADD reaches $V28D8 and
therefore which Layer1VramBuffer offset (= source) produces that cell.
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
        step_to(r, 80); step_to(o, 80)
        r.cmd('trace_vram_reset'); o.cmd('trace_vram_reset')
        r.cmd('trace_vram 28d8 28d8'); o.cmd('trace_vram 28d8 28d8')
        r.cmd('trace_vram 28d9 28d9'); o.cmd('trace_vram 28d9 28d9')
        step_to(r, 100); step_to(o, 100)
        rlog = r.cmd('get_vram_trace nostack').get('log', [])
        olog = o.cmd('get_vram_trace nostack').get('log', [])
        print(f'=== RECOMP $V28D8+$V28D9 writes: {len(rlog)} ===')
        for e in rlog:
            print(f'  f{e["f"]} ${e["adr"]}=0x{e["val"]} func={e["func"]}')
        print(f'\n=== ORACLE $V28D8+$V28D9 writes: {len(olog)} ===')
        for e in olog:
            print(f'  f{e["f"]} ${e["adr"]}=0x{e["val"]} func={e["func"]}')
    finally:
        r.close(); o.close(); _kill()


if __name__ == '__main__':
    main()
