"""Bug: koopa spawn — recomp's loop never finds a sprite to load.
Check Layer1XPos ($1A-$1B) which feeds the screen-boundary
calculation (LoadSprFromLevel line 5247: ADC DATA_02A7F9,Y).
If Layer1XPos differs, the boundary differs, sprites can't pass.
"""
import sys, pathlib, time, subprocess, socket
THIS_DIR = pathlib.Path(__file__).parent
sys.path.insert(0, str(THIS_DIR))
from harness import DebugClient, RECOMP_PORT  # noqa: E402

REPO = pathlib.Path(__file__).parent.parent.parent.parent
ORACLE_EXE = REPO / 'build' / 'bin-x64-Oracle' / 'smw.exe'


def _kill(): subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'], capture_output=True)


def launch():
    _kill(); time.sleep(0.5)
    subprocess.Popen([str(ORACLE_EXE), '--paused'], cwd=str(REPO),
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


def rb(c, cmd, addr, n=1):
    r = c.cmd(f'{cmd} 0x{addr:x} {n}')
    h = r.get('hex', '').replace(' ', '')
    return bytes.fromhex(h) if h else b''


def main():
    launch()
    c = DebugClient(RECOMP_PORT)
    try:
        c.cmd('pause')
        step_to(c, 250)
        target_mode = rb(c, 'read_ram', 0x100, 1)[0]
        for _ in range(60):
            if rb(c, 'emu_read_wram', 0x100, 1)[0] == target_mode: break
            c.cmd('emu_step 20')

        BYTES = [
            ('TrueFrame ($13)',   0x13, 1),
            ('TrueFrame ($14)',   0x14, 1),
            ('Layer1XPos ($1A)',  0x1A, 2),
            ('Layer1YPos ($1C)',  0x1C, 2),
            ('Layer1ScrollDir($1E)', 0x1E, 1),
            ('ScreenMode ($5B)',  0x5B, 1),
            ('SpriteDataPtr',     0xCE, 3),
        ]
        print(f'{"name":<24}  recomp        emu       diff')
        print('-' * 56)
        for label, addr, n in BYTES:
            r = rb(c, 'read_ram', addr, n)
            e = rb(c, 'emu_read_wram', addr, n)
            mark = '  <-- DIFF' if r != e else ''
            print(f'{label:<24}  {r.hex():<12}  {e.hex():<12}{mark}')

        # Now run a per-loop-iteration narrow trace: arm trace_wram_reads
        # on $1A-$1B (Layer1XPos read by ParseLevelSpriteList line 5246)
        # and capture what recomp reads vs. what the boundary should be.
        print('\nReads of Layer1XPos and TrueFrame from inside ParseLevelSpriteList:')
        c.cmd('trace_wram_reads_reset')
        c.cmd('trace_wram_reads 13 14')
        c.cmd('trace_wram_reads 1a 1b')
        c.cmd('step 20')
        time.sleep(0.3)
        r = c.cmd('get_wram_read_trace')
        log = r.get('log', [])
        # Just from ParseLevelSpriteList.
        psl = [x for x in log if 'ParseLevelSpriteList' in x.get('func','')
               or 'ParseLevelSpriteList' in x.get('parent','')]
        print(f'  Total reads: {len(log)}, from ParseLevelSpriteList: {len(psl)}')
        for e in psl[:10]:
            print(f"    f{e.get('f')} {e.get('adr')}={e.get('val')} w={e.get('w')} {e.get('func')}")

        c.cmd('continue')
    finally:
        c.close(); _kill()


if __name__ == '__main__':
    main()
