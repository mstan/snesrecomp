"""Bug #8 phase 16: check StandOnSolidSprite ($1471) on both sides at
the moment recomp's RunPlayerBlockCode_00EEE1 fires with Y=0x20.

Per SMWDisX line 12422-12429, Y=0x20 is loaded by CODE_00EE1D ONLY
when StandOnSolidSprite != 0 AND PlayerYSpeed+1 not negative. If
StandOnSolidSprite differs between recomp (non-zero) and emu (zero),
that's the upstream cause.
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


def rb(c, cmd, addr, w=1):
    r = c.cmd(f'{cmd} 0x{addr:x} {w}')
    h = r.get('hex','').replace(' ','')
    if not h: return None
    return int.from_bytes(bytes.fromhex(h)[:w],'little')


def main():
    launch()
    c = DebugClient(RECOMP_PORT)
    try:
        c.cmd('pause')
        step_to(c, 215)

        # Sync emu to GameMode=07.
        target_mode = rb(c, 'read_ram', 0x100)
        for _ in range(30):
            if rb(c, 'emu_read_wram', 0x100) == target_mode: break
            c.cmd('emu_step 20')

        rec_sosp = rb(c, 'read_ram', 0x1471)
        emu_sosp = rb(c, 'emu_read_wram', 0x1471)
        rec_y = rb(c, 'read_ram', 0x7d)
        emu_y = rb(c, 'emu_read_wram', 0x7d)
        rec_xspd = rb(c, 'read_ram', 0x7b)
        emu_xspd = rb(c, 'emu_read_wram', 0x7b)

        print(f'StandOnSolidSprite ($1471):  recomp=0x{rec_sosp:02x}  emu=0x{emu_sosp:02x}')
        print(f'PlayerYSpeed+1     ($7D):    recomp=0x{rec_y:02x}    emu=0x{emu_y:02x}')
        print(f'PlayerXSpeed+1     ($7B):    recomp=0x{rec_xspd:02x}    emu=0x{emu_xspd:02x}')
        print()
        # Also: ALL writers to $1471 over the f1-f215 window on recomp.
        c.cmd('trace_wram_reset')
        c.cmd('trace_wram 1471 1471')
        # Emu trace too.
        c.cmd('emu_wram_trace_reset')
        c.cmd('emu_wram_trace_add 1471 1471')
        # Need to step from a fresh state... actually we already stepped past.
        # Capture writes from this point forward.
        step_to(c, 230)
        c.cmd('emu_step 30')

        r = c.cmd('get_wram_trace')
        rec_log = r.get('log', [])
        print(f'Recomp writes to $1471 in last 15 frames: {len(rec_log)}')
        for e in rec_log[:10]:
            print(f'  f{e.get("f")}: $1471={e.get("val")} ({e.get("func")} <- {e.get("parent")})')

        r = c.cmd('emu_get_wram_trace')
        emu_log = r.get('log', [])
        print(f'\nEmu writes to $1471 in next 30 emu frames: {len(emu_log)}')
        for e in emu_log[:10]:
            print(f'  emuf{e.get("f")}: PC={e.get("pc")} {e.get("before")} -> {e.get("after")}')

        c.cmd('continue')
    finally:
        c.close(); _kill()


if __name__ == '__main__':
    main()
