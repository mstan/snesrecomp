"""Bug #8 phase 6: full WRAM diff between recomp (f100) and emu (+196).

Both sides have just finished the GameMode 4->5 transition. The full
WRAM delta tells us every byte that diverges, not just $72. One of
those bytes is the upstream cause of the $72 divergence — finding it
localizes the bug's root.

Usage from repo root:
    python snesrecomp/tests/l3/_probe_bug8_full_wram_diff.py
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


def dump_range(c, cmd, start, length):
    """Pull `length` bytes from `start` via successive reads. Each read
    can return up to 1024 bytes (0x400). Returns a flat bytes object."""
    CHUNK = 0x400
    out = bytearray()
    addr = start
    remaining = length
    while remaining > 0:
        n = min(remaining, CHUNK)
        r = c.cmd(f'{cmd} 0x{addr:x} {n}')
        hex_s = r.get('hex', '').replace(' ', '')
        if not hex_s:
            return bytes(out)
        chunk = bytes.fromhex(hex_s)
        out.extend(chunk)
        addr += len(chunk)
        remaining -= len(chunk)
    return bytes(out)


def main():
    launch()
    c = DebugClient(RECOMP_PORT)
    try:
        c.cmd('pause')
        step_to(c, 100)
        c.cmd('emu_step 196')

        # Sanity.
        rec_mode = int.from_bytes(dump_range(c, 'read_ram', 0x100, 1), 'little')
        emu_mode = int.from_bytes(dump_range(c, 'emu_read_wram', 0x100, 1), 'little')
        rec_72 = int.from_bytes(dump_range(c, 'read_ram', 0x72, 1), 'little')
        emu_72 = int.from_bytes(dump_range(c, 'emu_read_wram', 0x72, 1), 'little')
        print(f'Sync: recomp GameMode=0x{rec_mode:02x} $72=0x{rec_72:02x}  |  emu GameMode=0x{emu_mode:02x} $72=0x{emu_72:02x}')

        # Pull the full low-bank WRAM (bank 7E, $0000-$1FFF is the
        # gameplay-critical region). The 8KB high region is mostly
        # scratch/sprite tables — skip for now.
        print('\nDumping WRAM $00000-$01FFF on both sides (8 KB)...')
        rec_wram = dump_range(c, 'read_ram',      0x0000, 0x2000)
        emu_wram = dump_range(c, 'emu_read_wram', 0x0000, 0x2000)
        print(f'  recomp={len(rec_wram)} bytes, emu={len(emu_wram)} bytes')

        # Byte-by-byte diff.
        diffs = []
        for i in range(min(len(rec_wram), len(emu_wram))):
            if rec_wram[i] != emu_wram[i]:
                diffs.append((i, rec_wram[i], emu_wram[i]))

        print(f'\nTotal diverged bytes in $0-$1FFF: {len(diffs)}')
        if len(diffs) > 200:
            print(f'(too many to list; showing first 60 + $72 region)')
            display = diffs[:60]
            # Ensure $72 is visible.
            for d in diffs:
                if d[0] == 0x72 and d not in display:
                    display.append(d)
        else:
            display = diffs

        # Coalesce into runs for readability.
        print('\nDiverged address  recomp  emu')
        for addr, rv, ev in display:
            print(f'  $0{addr:04x}        0x{rv:02x}    0x{ev:02x}')

        # Key ZP + gameplay ranges breakdown.
        def count_in(lo, hi):
            return sum(1 for a, _, _ in diffs if lo <= a <= hi)
        print()
        print('Counts by region:')
        print(f'  $0000-$00FF (ZP)          : {count_in(0x0000, 0x00FF)}')
        print(f'  $0100-$01FF (low-RAM)     : {count_in(0x0100, 0x01FF)}')
        print(f'  $0200-$07FF (OAM/scratch) : {count_in(0x0200, 0x07FF)}')
        print(f'  $0800-$0FFF (misc)        : {count_in(0x0800, 0x0FFF)}')
        print(f'  $1000-$1FFF (game state)  : {count_in(0x1000, 0x1FFF)}')

        c.cmd('continue')
    finally:
        c.close(); _kill()


if __name__ == '__main__':
    main()
