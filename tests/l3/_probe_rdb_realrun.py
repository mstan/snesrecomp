"""Tier-1 litmus: natural game run, trace every WRAM write to L1VramBuffer
during frame-95 execution. Shows which function last writes each diffing
byte, including any overwrite that happens after BufferScrollingTiles_Layer1.
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
        f = c.cmd('frame').get('frame', 0)
        if f >= target: return f
        time.sleep(0.05)
    return c.cmd('frame').get('frame', 0)


def main():
    launch()
    c = DebugClient(RECOMP_PORT)
    try:
        c.cmd('pause')
        start_frame = int(sys.argv[1]) if len(sys.argv) > 1 else 94
        end_frame   = int(sys.argv[2]) if len(sys.argv) > 2 else 96
        step_to(c, start_frame)
        # Arm trace on the divergent region (and a wider sweep)
        c.cmd('trace_wram_reset')
        c.cmd('trace_wram 1c26 1c65')   # the diff hot-zone
        c.cmd('trace_wram 1be6 1ce5')   # full L1 buffer
        step_to(c, end_frame)

        trace = c.cmd('get_wram_trace')
        log = trace.get('log', [])
        print(f'captured {trace.get("entries")} writes (payload {len(log)})\n')

        # Group by address: show func of FIRST and LAST write per addr
        by_adr = {}
        for i, e in enumerate(log):
            a = int(e['adr'], 16)
            by_adr.setdefault(a, []).append((i, int(e['val'], 16), e['w'], e['func'], e['f']))

        # Focus on $1C3E-$1C51 (the first-divergent area from prior session)
        print('=== Writes to $1C3E-$1C51 (diff hot-zone) ===')
        for a in sorted(by_adr):
            if 0x1C3E <= a <= 0x1C51:
                writes = by_adr[a]
                print(f'\n  ${a:04x}  ({len(writes)} writes):')
                for idx, val, w, func, f in writes:
                    print(f'    [t{idx:>4}] f{f} val=0x{val:04x} w={w} func={func}')

        # Also find which FINAL value sits at $1C3E and who wrote it
        print('\n=== Final value + last-writer for $1C3E ===')
        if 0x1C3E in by_adr:
            last = by_adr[0x1C3E][-1]
            print(f'  last: idx={last[0]} val=0x{last[1]:04x} func={last[3]}')
    finally:
        c.close(); _kill()


if __name__ == '__main__':
    main()
