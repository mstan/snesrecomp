"""Bug #8 phase 13: identify what first writes PlayerYSpeed ($7D,
word at $7D:$7E) non-zero on recomp. If the same writer fires on emu
but at a later frame, it's a cadence/timing mismatch. If different
writers fire, it's a codegen/runner bug.
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

        c.cmd('trace_wram_reset')
        c.cmd('trace_wram 7d 7e')        # PlayerYSpeed word
        c.cmd('emu_wram_trace_reset')
        c.cmd('emu_wram_trace_add 7d 7e')

        step_to(c, 215)
        c.cmd('emu_step 400')   # push emu deep enough to see its trigger

        # Recomp: find the first write with a non-zero VALUE.
        r = c.cmd('get_wram_trace')
        rec_log = r.get('log', [])
        first_nz = None
        for e in rec_log:
            try:
                if int(e.get('val',''), 16) != 0:
                    first_nz = e; break
            except Exception: pass
        print(f'Recomp writes to $7D/$7E: {len(rec_log)} total')
        print(f'First NON-ZERO recomp write:')
        if first_nz:
            print(f'  f{first_nz.get("f")}: {first_nz.get("adr")}={first_nz.get("val")} '
                  f'({first_nz.get("func")} <- {first_nz.get("parent")})')
        else:
            print('  (no non-zero write observed)')

        # Show the 10 writes around the transition for context.
        print('\nAll recomp $7D/$7E writes:')
        for e in rec_log[:60]:
            print(f'  f{e.get("f"):4}: {e.get("adr")}={e.get("val")} '
                  f'(w={e.get("w")}, {e.get("func")} <- {e.get("parent")})')

        # Emu side.
        r = c.cmd('emu_get_wram_trace')
        emu_log = r.get('log', [])
        first_nz_emu = None
        for e in emu_log:
            try:
                if int(e.get('after',''), 16) != 0:
                    first_nz_emu = e; break
            except Exception: pass
        print(f'\nEmu writes to $7D/$7E: {len(emu_log)} total')
        print(f'First NON-ZERO emu write:')
        if first_nz_emu:
            print(f'  f{first_nz_emu.get("f")}: {first_nz_emu.get("adr")} '
                  f'{first_nz_emu.get("before")} -> {first_nz_emu.get("after")} '
                  f'(pc={first_nz_emu.get("pc")})')
        else:
            print('  (no non-zero write in window)')

        print('\nAll emu $7D/$7E writes:')
        for e in emu_log[:60]:
            print(f'  f{e.get("f"):4}: {e.get("adr")} '
                  f'{e.get("before")} -> {e.get("after")} (pc={e.get("pc")})')

        c.cmd('continue')
    finally:
        c.close(); _kill()


if __name__ == '__main__':
    main()
