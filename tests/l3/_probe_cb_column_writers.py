"""Trace $7E:$CB20-$CB5F writes f0->f96 and identify the function(s)
responsible for the column-row-0 single-byte writes at $CB20/$CB30/
$CB40/$CB50. Oracle fills entire 16-byte columns at these addresses;
recomp writes only the first byte. Whoever writes those first bytes
(or the function that *should* be writing the rest) is the caller bug.
"""
import sys, pathlib, time, subprocess, socket
from collections import Counter, defaultdict
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
        step_to(c, 0)
        c.cmd('trace_wram_reset')
        # Trace the full Map16Lo region around the diff site, both banks.
        c.cmd('trace_wram cb00 cb80')
        c.cmd('trace_wram 1cb00 1cb80')
        step_to(c, 96)

        trace = c.cmd('get_wram_trace')
        log = trace.get('log', [])
        entries = trace.get('entries', 0)
        print(f'Captured {entries} writes (log payload {len(log)})\n')

        # All writes, grouped by function and address.
        per_addr = defaultdict(list)  # addr -> [(frame, val, func, width)]
        per_func = Counter()
        for e in log:
            a = int(e['adr'], 16)
            per_addr[a].append((e['f'], int(e['val'], 16), e['func'], e['w']))
            per_func[e['func']] += 1

        print('=== Top writers to $CB00-$CB80 (both banks) ===')
        for fn, n in per_func.most_common(20):
            print(f'  {n:>6}  {fn}')

        print('\n=== Writes per address (chronological) ===')
        for a in sorted(per_addr.keys()):
            evs = per_addr[a]
            tag = ''
            if (a & 0xF) == 0:
                tag = '  <-- COLUMN ROW 0'
            print(f'\n  ${a:05x}{tag}  ({len(evs)} writes)')
            # Show first 6, last 2
            for ev in evs:
                print(f'    f{ev[0]} val=0x{ev[1]:02x} w={ev[3]} fn={ev[2]}')
    finally:
        c.close(); _kill()


if __name__ == '__main__':
    main()
