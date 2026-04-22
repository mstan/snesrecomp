"""Bug #8 phase 18: read recomp's tracked A at every $EF60 block
entry. The 'a' field in the block-trace entry IS the value about to
be stored to $7D by the STA at $EF60.

If a=0 every frame: STA writes 0, the accumulation is downstream
(HandlePlayerPhysics_D930 firing post-STA on recomp).

If a=6 (or non-zero) every frame: STA writes the wrong value,
codegen bug at the EF60 emission OR upstream branch evaluation.
"""
import sys, pathlib, time, subprocess, socket
THIS_DIR = pathlib.Path(__file__).parent
sys.path.insert(0, str(THIS_DIR))
from harness import DebugClient, RECOMP_PORT  # noqa: E402

REPO = pathlib.Path(__file__).parent.parent.parent.parent
ORACLE_EXE = REPO / 'build' / 'bin-x64-Oracle' / 'smw.exe'


def _kill():
    subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'], capture_output=True)


def launch():
    _kill(); time.sleep(0.5)
    subprocess.Popen([str(ORACLE_EXE), '--paused'],
                     cwd=str(REPO),
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    deadline = time.time() + 20
    while time.time() < deadline:
        try:
            s = socket.create_connection(('127.0.0.1', RECOMP_PORT), timeout=0.3)
            s.close(); return
        except OSError:
            time.sleep(0.2)
    raise RuntimeError('no TCP connect')


def step_to(c, target):
    cur = c.cmd('frame').get('frame', 0)
    if cur >= target: return cur
    c.cmd(f'step {target - cur}')
    deadline = time.time() + 60
    while time.time() < deadline:
        if c.cmd('frame').get('frame', 0) >= target: return target
        time.sleep(0.03)


def main():
    launch()
    c = DebugClient(RECOMP_PORT)
    try:
        c.cmd('pause')
        c.cmd('trace_blocks_reset')
        c.cmd('trace_blocks')

        # Trace all $7D writes too so we can correlate.
        c.cmd('trace_wram_reset')
        c.cmd('trace_wram 7d 7d')

        step_to(c, 215)

        # Fetch all block hits at $EF60.
        r = c.cmd('get_block_trace pc_lo=0xef60 pc_hi=0xef60')
        ef60 = r.get('log', [])
        print(f'$EF60 entries (last 15 frames): {len(ef60)}')
        print(' frame | A      | X      | Y      | (A is value about to be STA-d to $7D)')
        for e in ef60[-15:]:
            print(f' {e.get("f"):4d}  | {e.get("a"):>6} | {e.get("x"):>6} | {e.get("y"):>6}')

        # Confirm $7D writes order in the same window.
        r = c.cmd('get_wram_trace')
        log = r.get('log', [])
        print(f'\nAll $7D writes in same window ({len(log)} total):')
        for e in log:
            print(f' f{e.get("f"):4d}: $7D={e.get("val")} (w={e.get("w")}, '
                  f'{e.get("func")} <- {e.get("parent")})')

        # If A at EF60 == 0 always, AND $7D ends up nonzero in the trace,
        # the accumulation is downstream of EF60.
        if ef60:
            ef60_as = set(e.get('a') for e in ef60[-10:])
            print(f'\nDistinct A values at $EF60 (last 10 frames): {ef60_as}')
            if ef60_as == {'0x0000'}:
                print('Verdict: STA at $EF60 writes 0 every frame. The accumulation MUST come')
                print('         from a write AFTER $EF60 in the per-frame sequence.')
                print('         Suspect: HandlePlayerPhysics_D930 firing when ROM-correct')
                print('         conditions would have it skipped.')
            else:
                print(f'Verdict: STA at $EF60 writes nonzero values: {ef60_as}')
                print('         Either codegen bug at $EF60 or upstream branch evaluation issue.')
                print('         Investigate the predecessor block of $EF60 to see how A got nonzero.')

        c.cmd('continue')
    finally:
        c.close(); _kill()


if __name__ == '__main__':
    main()
