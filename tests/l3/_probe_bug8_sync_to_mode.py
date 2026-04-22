"""Bug #8 investigation, phase 2: sync emu to recomp's GameMode.

Recomp fast-forwards through boot; snes9x runs at hardware rate. This
probe uses emu_step to catch snes9x up until it reaches the same
GameMode value as recomp. Once synced, compares $72 and other
Mario-state bytes frame-by-frame to find the exact moment emu clears
$72 that recomp doesn't.

Usage from repo root:
    python snesrecomp/tests/l3/_probe_bug8_sync_to_mode.py
"""
import sys, pathlib, time, subprocess, socket
THIS_DIR = pathlib.Path(__file__).parent
sys.path.insert(0, str(THIS_DIR))
from harness import DebugClient, RECOMP_PORT  # noqa: E402

REPO = pathlib.Path(__file__).parent.parent.parent.parent
ORACLE_EXE = REPO / 'build' / 'bin-x64-Oracle' / 'smw.exe'

GAME_MODE = 0x100  # SMW's game-mode byte
PLAYER_IN_AIR = 0x72

TARGET_MODE_RECOMP_FRAME = 100   # recomp at GameMode=$05 here (title-Mario-animation)


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


def rb(c, cmd, addr, width=1):
    r = c.cmd(f'{cmd} 0x{addr:x} {width}')
    hex_s = r.get('hex', '').replace(' ', '')
    if not hex_s:
        return None
    b = bytes.fromhex(hex_s)
    return int.from_bytes(b[:width], 'little') if b else None


def step_to(c, target):
    cur = c.cmd('frame').get('frame', 0)
    if cur >= target:
        return cur
    c.cmd(f'step {target - cur}')
    deadline = time.time() + 60
    while time.time() < deadline:
        if c.cmd('frame').get('frame', 0) >= target:
            return target
        time.sleep(0.03)


def main():
    launch()
    c = DebugClient(RECOMP_PORT)
    try:
        c.cmd('pause')

        step_to(c, TARGET_MODE_RECOMP_FRAME)
        rec_mode = rb(c, 'read_ram', GAME_MODE, 1)
        rec_72 = rb(c, 'read_ram', PLAYER_IN_AIR, 1)
        print(f'Recomp at f{TARGET_MODE_RECOMP_FRAME}: GameMode=0x{rec_mode:02x}, $72=0x{rec_72:02x}')

        emu_mode = rb(c, 'emu_read_wram', GAME_MODE, 1)
        emu_72 = rb(c, 'emu_read_wram', PLAYER_IN_AIR, 1)
        print(f'Emu   (same frame):          GameMode=0x{emu_mode:02x}, $72=0x{emu_72:02x}')
        print()

        # Walk emu forward in chunks until GameMode matches recomp, or give up.
        print('Walking emu forward until GameMode matches...')
        print('  emu-frames-advanced | emu GameMode | emu $72')
        total_emu_extra = 0
        chunk = 30
        for step in range(40):
            c.cmd(f'emu_step {chunk}')
            total_emu_extra += chunk
            em = rb(c, 'emu_read_wram', GAME_MODE, 1)
            e72 = rb(c, 'emu_read_wram', PLAYER_IN_AIR, 1)
            print(f'    +{total_emu_extra:5d}           |   0x{em:02x}       |  0x{e72:02x}')
            if em == rec_mode:
                print(f'\nMATCH: emu caught up to GameMode=0x{em:02x} after +{total_emu_extra} frames')
                # Compare $72 now.
                print(f'  recomp $72 = 0x{rec_72:02x}')
                print(f'  emu    $72 = 0x{e72:02x}')
                if rec_72 != e72:
                    print(f'  *** BUG #8 REPRODUCED: $72 diverges at GameMode=0x{em:02x} ***')
                    # Dump CPU regs on emu side.
                    r = c.cmd('emu_cpu_regs')
                    print(f'  emu CPU at divergence: {r}')
                else:
                    print('  No divergence at this sync point.')
                break
            if em > rec_mode:
                print(f'\nOVERSHOT: emu reached GameMode=0x{em:02x} (> recomp 0x{rec_mode:02x}) at +{total_emu_extra}')
                break
        else:
            print(f'\nTIMEOUT: emu still at GameMode=0x{em:02x} after +{total_emu_extra} frames')

        c.cmd('continue')
    finally:
        c.close(); _kill()


if __name__ == '__main__':
    main()
