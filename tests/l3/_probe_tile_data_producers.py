"""Wide WRAM audit: trace every writer to $7E:$C800-$CF00 and
$7F:$C800-$CF00 during real-game frames f94->f96. Both ranges are
where iter 31's tile lookup reads from. If these are populated by
recomp-emitted functions, state is ROM-accurate. If populated by
hand-written HLE scaffolding, audit against SMWDisX.
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
        step_to(c, 80)
        c.cmd('trace_wram_reset')
        # Bank 7E range (0xC800-0xCF00)
        c.cmd('trace_wram c800 cf00')
        # Bank 7F range (0x1C800-0x1CF00)
        c.cmd('trace_wram 1c800 1cf00')
        step_to(c, 96)

        trace = c.cmd('get_wram_trace')
        log = trace.get('log', [])
        entries = trace.get('entries', 0)
        print(f'Captured {entries} writes (log payload {len(log)})\n')

        # Who wrote to each bank? Count by function.
        bank7e = Counter()
        bank7f = Counter()
        for e in log:
            a = int(e['adr'], 16)
            if 0xC800 <= a <= 0xCF00:
                bank7e[e['func']] += 1
            elif 0x1C800 <= a <= 0x1CF00:
                bank7f[e['func']] += 1

        print('=== BANK $7E:$C800-$CF00 writers (all frames captured) ===')
        for func, n in bank7e.most_common(20):
            print(f'  {n:>6}  {func}')

        print('\n=== BANK $7F:$C800-$CF00 writers ===')
        for func, n in bank7f.most_common(20):
            print(f'  {n:>6}  {func}')

        # For iter 31's specific target ($7E:CB17 and $7F:CB17), list exact writes
        for target in [0xCB17, 0x1CB17]:
            print(f'\n=== Writes to ${target:05x} specifically ===')
            for e in log:
                a = int(e['adr'], 16)
                if a == target:
                    print(f'  f{e["f"]} val=0x{e["val"]} w={e["w"]} func={e["func"]}')
    finally:
        c.close(); _kill()


if __name__ == '__main__':
    main()
