"""Bug #8 phase 12: trace every write to PlayerYPosNext ($96) on
both sides and find the earliest frame where they diverge.

From phase 11 we know recomp ends up with Mario's Y = 0x0160 vs
emu's 0x0150 at the same GameMode=07 sync point. That 0x10 delta IS
the '1-block-under' bug. The first divergent $96 write tells us
exactly which function, on which side, introduced the error.

Strategy: Tier-1 trace $96 on recomp (full window), emu_wram_trace
$96 on emu (full window). Print both timelines side by side in
time order. The first pair where value differs is the root.
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

        # Arm traces on $96 / $97 (PlayerYPosNext word) on BOTH sides
        # before any stepping, so we capture from frame 1.
        c.cmd('trace_wram_reset')
        c.cmd('trace_wram 96 97')
        c.cmd('emu_wram_trace_reset')
        c.cmd('emu_wram_trace_add 96 97')

        # Let recomp reach the sync point. This also steps emu 215 frames
        # via the main-loop tick.
        step_to(c, 215)
        # Advance emu further to reach its GameMode=07 state (prior probe
        # needed +200 extra emu-steps past the recomp-side step_to(215)).
        c.cmd('emu_step 200')

        # Recomp writes.
        r = c.cmd('get_wram_trace')
        rec_log = r.get('log', [])
        print(f'Recomp writes to $96/$97 (f1-f215), total={len(rec_log)}:')
        print(' frame | adr    | val    | w | func')
        for e in rec_log[:30]:
            print(f' {e.get("f"):4d}  | {e.get("adr")} | {e.get("val")} | {e.get("w")} | '
                  f'{e.get("func")} <- {e.get("parent")}')
        if len(rec_log) > 30:
            print(f' ... and {len(rec_log)-30} more')

        # Emu writes.
        r = c.cmd('emu_get_wram_trace')
        emu_log = r.get('log', [])
        print(f'\nEmu writes to $96/$97 (emu frames 1-~415), total={len(emu_log)}:')
        print(' frame |    PC      | adr    | before -> after | bank')
        for e in emu_log[:30]:
            print(f' {e.get("f"):4}  | {e.get("pc"):10} | {e.get("adr")} | '
                  f'{e.get("before")} -> {e.get("after")}     | {e.get("bank_src")}')
        if len(emu_log) > 30:
            print(f' ... and {len(emu_log)-30} more')

        # Correlate: find the first pair where recomp and emu disagree
        # about $96's value. We don't have perfect frame-sync so we'll
        # order by value-change events instead.
        print('\nValue history comparison:')
        print('  recomp: adr -> val sequence')
        rec_seq = [(e.get('adr'), e.get('val'), e.get('w'), e.get('f'), e.get('func'))
                   for e in rec_log]
        for a, v, w, f, fn in rec_seq[:20]:
            print(f'    f{f:4d} {a}={v} (w={w}, {fn})')
        print('  emu: adr -> val sequence')
        emu_seq = [(e.get('adr'), e.get('before'), e.get('after'), e.get('f'), e.get('pc'))
                   for e in emu_log]
        for a, b, aa, f, pc in emu_seq[:20]:
            print(f'    emuf{f:4} {a}: {b} -> {aa} (pc={pc})')

        c.cmd('continue')
    finally:
        c.close(); _kill()


if __name__ == '__main__':
    main()
