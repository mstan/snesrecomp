"""Bug: koopas / enemies don't spawn during title-screen attract demo.

SMW's title demo runs a short scripted level with koopas Mario stomps.
If no sprites are active on recomp during that demo, the spawn
pipeline fired differently than the ROM.

Approach:
  1. Step both sides into the GM=07 title demo phase.
  2. Dump SpriteStatus[0..11] ($14C8+0..11) on both sides, across
     frames f200, f220, f250, f300.
  3. Compare: does recomp have any non-zero slot? Does emu? If emu
     has sprites but recomp doesn't, walk back to find the missing
     init.
  4. Dump SpriteNumber[0..11] ($9E is DP — actually look at
     $14C8 region for the main sprite table).

Sprite slot range (from SMW_U.sym + SMWDisX):
  $14C8-$14D3  SpriteStatus   (12 slots)
  $14E0-$14EB  SpriteYPosLow / SpriteMisc ... actually let's just
                diff the whole $14C8-$1600 block.
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


def read_bytes(c, cmd, addr, n):
    r = c.cmd(f'{cmd} 0x{addr:x} {n}')
    h = r.get('hex', '').replace(' ', '')
    return bytes.fromhex(h) if h else b''


def dump_sprite_slots(c, cmd, label):
    status = read_bytes(c, cmd, 0x14C8, 12)     # 12 slots
    sprnum = read_bytes(c, cmd, 0x9E, 12)       # sprite IDs (DP)
    ypos_l = read_bytes(c, cmd, 0x14D4, 12)     # Y low
    xpos_l = read_bytes(c, cmd, 0x14E0, 12)     # X low
    print(f'{label:>10}  slot:  ' + ' '.join(f'{i:>4}' for i in range(12)))
    print(f'{"":10}  stat:  ' + ' '.join(f'0x{b:02x}' for b in status))
    print(f'{"":10}  num :  ' + ' '.join(f'0x{b:02x}' for b in sprnum))
    print(f'{"":10}  ypos:  ' + ' '.join(f'0x{b:02x}' for b in ypos_l))
    print(f'{"":10}  xpos:  ' + ' '.join(f'0x{b:02x}' for b in xpos_l))
    return status


def main():
    launch()
    c = DebugClient(RECOMP_PORT)
    try:
        c.cmd('pause')

        # Arm wide WRAM trace on the sprite-table region so we can
        # walk back if no writes fire.
        c.cmd('trace_wram_reset')
        c.cmd('trace_wram 14c8 1600')

        # Sync both sides: recomp to f300 (deep into attract demo),
        # emu then walked forward with emu_step until its GameMode
        # matches.
        step_to(c, 300)

        # Walk emu to GameMode=07 (title demo).
        def gm(c, side):
            return int.from_bytes(read_bytes(c, side, 0x100, 1), 'little') if read_bytes(c, side, 0x100, 1) else 0
        target_mode = gm(c, 'read_ram')
        print(f'Recomp GameMode=0x{target_mode:02x}')
        for _ in range(60):
            if gm(c, 'emu_read_wram') == target_mode: break
            c.cmd('emu_step 20')
        print(f'Emu GameMode=0x{gm(c, "emu_read_wram"):02x}\n')

        print('Sprite slot contents (12 slots):')
        rec_status = dump_sprite_slots(c, 'read_ram', 'RECOMP')
        print()
        emu_status = dump_sprite_slots(c, 'emu_read_wram', 'EMU')
        print()

        any_rec = any(b != 0 for b in rec_status)
        any_emu = any(b != 0 for b in emu_status)
        print(f'Any active on recomp? {any_rec}')
        print(f'Any active on emu?    {any_emu}')

        if not any_rec and any_emu:
            print('\n*** BUG CONFIRMED: emu has active sprites, recomp has none. ***')
            # Dump write-trace for the sprite region.
            r = c.cmd('get_wram_trace')
            log = r.get('log', [])
            # Filter to the sprite-table range.
            rel = [e for e in log
                   if 0x14c8 <= int(e.get('adr','0x0'), 16) < 0x1600]
            print(f'\nRecomp writes to $14C8-$1600 total={len(rel)}:')
            # First 30 writes and the slot affected.
            for e in rel[:30]:
                addr = int(e.get('adr'), 16)
                slot_guess = ''
                if 0x14c8 <= addr <= 0x14d3: slot_guess = f' (SpriteStatus[{addr-0x14c8}])'
                elif 0x14d4 <= addr <= 0x14df: slot_guess = f' (SpriteYLo[{addr-0x14d4}])'
                elif 0x14e0 <= addr <= 0x14eb: slot_guess = f' (SpriteXLo[{addr-0x14e0}])'
                print(f"  f{e.get('f'):>3} bi={e.get('bi')} {e.get('adr')}={e.get('val')} "
                      f"w={e.get('w')} {e.get('func')} <- {e.get('parent')}{slot_guess}")
        elif any_rec and any_emu:
            print('\nBoth sides have sprites. Bug may be mis-positioned rather than missing.')
        elif any_rec and not any_emu:
            print('\nRecomp has sprites, emu does not — unexpected; emu may still be in earlier GM.')
        else:
            print('\nNeither side has sprites. Step deeper into the demo.')

        c.cmd('continue')
    finally:
        c.close(); _kill()


if __name__ == '__main__':
    main()
