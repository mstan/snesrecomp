"""Bug: koopa spawn — recomp's ParseLevelSpriteList exits at the
loop's first byte (BEQ Return02A84B on $FF). Check whether
SpriteDataPtr ($CE-$D0) and the data it points to differ between
recomp and emu.
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
        # Sync emu to GameMode=07.
        target_mode = rb(c, 'read_ram', 0x100, 1)[0]
        for _ in range(60):
            if rb(c, 'emu_read_wram', 0x100, 1)[0] == target_mode: break
            c.cmd('emu_step 20')

        # SpriteDataPtr ($CE = lo, $CF = hi, $D0 = bank).
        rec_ptr = rb(c, 'read_ram', 0xCE, 3)
        emu_ptr = rb(c, 'emu_read_wram', 0xCE, 3)
        print(f'SpriteDataPtr ($CE-$D0):')
        print(f'  recomp: {rec_ptr.hex()}  bank={rec_ptr[2]:02x} addr=0x{rec_ptr[1]:02x}{rec_ptr[0]:02x}')
        print(f'  emu:    {emu_ptr.hex()}  bank={emu_ptr[2]:02x} addr=0x{emu_ptr[1]:02x}{emu_ptr[0]:02x}')

        # If the pointers match, dump the first ~16 bytes of the sprite
        # data on both sides via cpu_read.
        rec_addr_24 = (rec_ptr[2] << 16) | (rec_ptr[1] << 8) | rec_ptr[0]
        emu_addr_24 = (emu_ptr[2] << 16) | (emu_ptr[1] << 8) | emu_ptr[0]
        print(f'\nFirst 16 bytes at SpriteDataPtr (read via emu_cpu_read):')
        # Read each byte via the emu bus (works for ROM addresses).
        emu_bytes = []
        for off in range(16):
            r = c.cmd(f'emu_read_wram {(emu_addr_24 + off) & 0x1ffff:x} 1') if (emu_addr_24 < 0x800000) else None
            # Better: use emu_cpu_read - but we don't have a TCP cmd for that.
            # The data is in ROM; emu's snes9x_bridge_cpu_read reads any
            # 24-bit bus address. We don't have a cmd for it... add one or
            # work around: check if SpriteDataPtr points into bank 7E (WRAM).
            emu_bytes.append('?')

        print(f'  emu addr_24 = 0x{emu_addr_24:06x}')
        print(f'  recomp addr_24 = 0x{rec_addr_24:06x}')
        if emu_addr_24 == rec_addr_24:
            print('  pointers MATCH on both sides')
        else:
            print('  *** pointers DIFFER ***')

        # The data is in ROM — can read from the file directly.
        rom = (REPO / 'smw.sfc').read_bytes()
        # LoROM mapping: bank N, addr $8000-$FFFF = ROM offset (N*0x8000) + (addr-0x8000)
        def lorom_off(addr24):
            bank = (addr24 >> 16) & 0x7F
            addr = addr24 & 0xFFFF
            if addr < 0x8000:
                return None
            return bank * 0x8000 + (addr - 0x8000)

        def read_rom(addr24, n):
            off = lorom_off(addr24)
            if off is None or off + n > len(rom): return None
            return rom[off:off + n]

        emu_rom = read_rom(emu_addr_24, 16)
        rec_rom = read_rom(rec_addr_24, 16)
        if emu_rom: print(f'  emu_rom[0..16] (LoROM) = {emu_rom.hex()}')
        if rec_rom: print(f'  rec_rom[0..16] (LoROM) = {rec_rom.hex()}')

        # Also the byte read by the loop is [SpriteDataPtr],Y where Y=1.
        # If that byte is $FF, BEQ taken -> return.
        if emu_rom: print(f'\n  emu byte at offset 1 ([SpriteDataPtr],Y=1) = 0x{emu_rom[1]:02x}')
        if rec_rom: print(f'  rec byte at offset 1 ([SpriteDataPtr],Y=1) = 0x{rec_rom[1]:02x}')

        c.cmd('continue')
    finally:
        c.close(); _kill()


if __name__ == '__main__':
    main()
