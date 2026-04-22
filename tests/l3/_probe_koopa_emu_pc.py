"""Bug: koopa spawn missing on recomp. Use emu's per-write PC trace
to find which ROM PC writes SpriteStatus[9] = 0x08. Then check if
recomp ever executes the same code region.
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

        # Arm emu write trace on the SpriteStatus + SpriteNumber region.
        c.cmd('emu_wram_trace_reset')
        c.cmd('emu_wram_trace_add 14c8 14d3')   # SpriteStatus[0..11]

        # Drive both sides forward.
        step_to(c, 300)

        # Get emu writes.
        r = c.cmd('emu_get_wram_trace')
        log = r.get('log', [])
        print(f'Emu writes to SpriteStatus[0..11] over the run: {len(log)}')
        # Show the first non-zero writes (initial spawns).
        nz = [e for e in log if e.get('after') != '0x00']
        print(f'  Non-zero (sprite SPAWN) writes: {len(nz)}')
        for e in nz[:10]:
            slot = int(e.get('adr'), 16) - 0x14c8
            print(f"    emu_f{e.get('f')} PC={e.get('pc')}  SpriteStatus[{slot}] "
                  f"{e.get('before')} -> {e.get('after')}  bank={e.get('bank_src')}")

        if not nz:
            print('  (No spawn writes captured — emu may not have hit the spawn PC yet.)')
            print('  Try stepping further:')
            c.cmd('emu_step 200')
            r = c.cmd('emu_get_wram_trace')
            log = r.get('log', [])
            nz = [e for e in log if e.get('after') != '0x00']
            print(f'  After +200 emu frames: spawn writes = {len(nz)}')
            for e in nz[:5]:
                slot = int(e.get('adr'), 16) - 0x14c8
                print(f"    emu_f{e.get('f')} PC={e.get('pc')}  SpriteStatus[{slot}] "
                      f"{e.get('before')} -> {e.get('after')}")

        if nz:
            # Analyze: take the first spawn PC and see if recomp's
            # block trace ever hits the surrounding PC range.
            first = nz[0]
            spawn_pc = int(first.get('pc'), 16)
            print(f'\nFirst spawn PC on emu: 0x{spawn_pc:06x}')
            print('Checking if recomp ever entered a block in this PC vicinity (+-128 bytes)...')
            # Use Tier 2 block trace. Arm and step.
            c.cmd('trace_blocks_reset')
            c.cmd('trace_blocks')
            step_to(c, 350)
            lo = max(0, spawn_pc - 0x80)
            hi = spawn_pc + 0x80
            r = c.cmd(f'get_block_trace pc_lo=0x{lo:x} pc_hi=0x{hi:x}')
            blocks = r.get('log', [])
            print(f'  Recomp blocks in $0x{lo:06x}-$0x{hi:06x}: {len(blocks)}')
            if blocks:
                print('  Sample:')
                for b in blocks[:6]:
                    print(f"    f{b.get('f')} bi={b.get('bi')} {b.get('pc')} {b.get('func')}")
            else:
                print('  *** Recomp never reaches the spawn PC region. ***')

        c.cmd('continue')
    finally:
        c.close(); _kill()


if __name__ == '__main__':
    main()
