"""Force specific Layer1TileUp value at f95 state, invoke
BufferScrollingTiles_Layer1, observe output. Isolates input-sensitivity
of the bug.
"""
import sys, pathlib, time, subprocess, socket
THIS_DIR = pathlib.Path(__file__).parent
sys.path.insert(0, str(THIS_DIR))
from harness import RECOMP_EXE, RECOMP_PORT, DebugClient  # noqa: E402


def _kill(): subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'], capture_output=True)


def launch():
    _kill(); time.sleep(0.5)
    subprocess.Popen([str(RECOMP_EXE), '--paused'],
        cwd=str(RECOMP_EXE.parent.parent.parent),
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    deadline = time.time() + 15
    while time.time() < deadline:
        try:
            s = socket.create_connection(('127.0.0.1', RECOMP_PORT), timeout=0.3); s.close(); break
        except OSError: time.sleep(0.2)
    time.sleep(0.3)


def step_to(c, target):
    base = c.cmd('frame').get('frame', 0)
    if base >= target: return base
    c.cmd(f'step {target - base}')
    deadline = time.time() + 60
    while time.time() < deadline:
        f = c.cmd('frame').get('frame', 0)
        if f >= target: return f
        time.sleep(0.05)
    return c.cmd('frame').get('frame', 0)


def rb(c, addr, n):
    r = c.cmd(f'read_ram 0x{addr:x} {n}')
    return bytes.fromhex(r.get('hex', '').replace(' ', ''))


def main():
    forced_tileup = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0xFFF8
    forced_55 = int(sys.argv[2], 16) if len(sys.argv) > 2 else None
    forced_tiledown = int(sys.argv[3], 16) if len(sys.argv) > 3 else None
    launch()
    r = DebugClient(RECOMP_PORT)
    try:
        r.cmd('pause')
        step_to(r, 95)

        # Force Layer1TileUp = forced_tileup (16-bit at $0045)
        lo = forced_tileup & 0xff
        hi = (forced_tileup >> 8) & 0xff
        r.cmd(f'write_ram 45 {lo:02x}')
        r.cmd(f'write_ram 46 {hi:02x}')
        if forced_55 is not None:
            r.cmd(f'write_ram 55 {forced_55:02x}')
        if forced_tiledown is not None:
            r.cmd(f'write_ram 47 {forced_tiledown & 0xff:02x}')
            r.cmd(f'write_ram 48 {(forced_tiledown >> 8) & 0xff:02x}')
        # Verify
        b = rb(r, 0x45, 4)
        s55 = rb(r, 0x55, 1)
        print(f'State: $45={b[0]:02x} $46={b[1]:02x} $47={b[2]:02x} $48={b[3]:02x} $55={s55[0]:02x}')

        # Zero the L1 buffer so we can see what invoke writes
        for i in range(0, 256):
            r.cmd(f'write_ram {0x1BE6+i:x} 00')
        pre = rb(r, 0x1BE6, 256)
        print(f'Pre-invoke buffer all zero: {all(b==0 for b in pre)}')

        # Invoke
        rr = r.cmd('invoke_recomp BufferScrollingTiles_Layer1')
        print(f'Invoke response: {rr}')

        # Pre-dump WRAM regions we'll read from
        c9b0 = rb(r, 0xC9B0, 64)
        print(f'Pre: $7E:$C9B0-$C9EF = {c9b0.hex()}')
        cdb0 = rb(r, 0xCDB0, 64)
        print(f'Pre: $7E:$CDB0-$CDEF = {cdb0.hex()}')

        # Also dump Map16LowPtr / Map16HighPtr and _A
        print(f'Map16LowPtr ($6B-$6D): {rb(r, 0x6B, 3).hex()}')
        print(f'Map16HighPtr ($6E-$70): {rb(r, 0x6E, 3).hex()}')
        print(f'_A ($0A-$0C): {rb(r, 0x0A, 3).hex()}')
        print(f'_8 ($08-$09): {rb(r, 0x08, 2).hex()}')

        # Dump buffer
        post = rb(r, 0x1BE6, 256)
        print(f'\nPost-invoke buffer ($1BE6-$1CE5):')
        for i in range(0, len(post), 16):
            print(f'  ${0x1BE6+i:04x}: {" ".join(f"{x:02x}" for x in post[i:i+16])}')

        # Count blank tiles (0xf810) vs non-blank tiles
        blanks = 0; non_blanks = 0; zeros = 0
        for i in range(0, 256, 2):
            w = post[i] | (post[i+1] << 8)
            if w == 0: zeros += 1
            elif w == 0x10F8: blanks += 1
            else: non_blanks += 1
        print(f'\nWord summary: blanks(0x10F8)={blanks}, zeros={zeros}, non-blank-real={non_blanks}')
    finally:
        r.close(); _kill()


if __name__ == '__main__':
    main()
