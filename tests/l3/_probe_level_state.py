"""Probe level-building state at attract-mode frame 96.

Checks surface-level upstream state to narrow "only first strip column
renders":
- Layer1VramAddr ($1BE4-$1BE5): VRAM dest for current upload.
- ScreenMode ($5B): vertical-level flag etc.
- $13: global frame counter (sanity check that we're really at same frame).
- $14: local frame counter.
- $71: current game mode.
- $1BE6-$1CE5: the 4-strip buffer (for pattern analysis).
- Mario's X/Y position ($94/$96, $98/$9A for 16-bit precision).
- Layer 1 scroll X/Y ($1A/$1C, $1C/$1E).
- $13C6: upload mode (0x08 = normal, chosen by NMI branch).
- $18C5: APUI02 mirror.

Then a Map16 probe: the decoded block map at $7EC800+ (the physics + render
source). If this is empty/zeroed, ground render AND collision both fail.
"""
import sys
import pathlib
import time
import subprocess
import socket

THIS_DIR = pathlib.Path(__file__).parent
sys.path.insert(0, str(THIS_DIR))

from harness import RECOMP_EXE, ORACLE_EXE, RECOMP_PORT, ORACLE_PORT  # noqa: E402
from harness import DebugClient  # noqa: E402


def _kill():
    subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'],
                   capture_output=True, check=False)


def _ports_ready():
    for port in (RECOMP_PORT, ORACLE_PORT):
        try:
            s = socket.create_connection(('127.0.0.1', port), timeout=0.3)
            s.close()
        except (OSError, ConnectionRefusedError):
            return False
    return True


def launch_both():
    _kill()
    time.sleep(0.5)
    subprocess.Popen(
        [str(RECOMP_EXE), '--paused'],
        cwd=str(RECOMP_EXE.parent.parent.parent),
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    subprocess.Popen(
        [str(ORACLE_EXE), '--paused', '--theirs'],
        cwd=str(ORACLE_EXE.parent.parent.parent),
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    deadline = time.time() + 15
    while time.time() < deadline:
        if _ports_ready():
            time.sleep(0.3)
            return
        time.sleep(0.2)
    raise RuntimeError('timeout waiting for ports')


def dump_ram(client, addr, length):
    hex_parts = []
    off = 0
    while off < length:
        chunk = min(256, length - off)
        r = client.cmd(f'read_ram 0x{addr+off:x} {chunk}')
        hex_parts.append(r.get('hex', ''))
        off += chunk
    return bytes.fromhex(''.join(hex_parts))


POINTS = [
    ('GameMode ($71)', 0x0071, 1),
    ('GlobalFrameCtr ($13)', 0x0013, 1),
    ('LocalFrameCtr ($14)', 0x0014, 1),
    ('CurrentLevelLayer1XPos ($1A)', 0x001A, 2),
    ('CurrentLevelLayer1YPos ($1C)', 0x001C, 2),
    ('CurrentLevelLayer2XPos ($1E)', 0x001E, 2),
    ('CurrentLevelLayer2YPos ($20)', 0x0020, 2),
    ('ScreenMode ($5B)', 0x005B, 1),
    ('MarioXLo ($94)', 0x0094, 2),
    ('MarioYLo ($96)', 0x0096, 2),
    ('IRQNMICommand ($D9B)', 0x0D9B, 1),
    ('APUI02 mirror ($18C5)', 0x18C5, 1),
    ('EndLevelTimer ($1493)', 0x1493, 1),
    ('Map16UploadFlag ($13C6)', 0x13C6, 1),
    ('Layer1VramAddr ($1BE4)', 0x1BE4, 2),
    ('Layer1VramBuffer[0..16] ($1BE6)', 0x1BE6, 16),
    ('Layer1VramBuffer[16..32]', 0x1BF6, 16),
    ('Layer1VramBuffer[64..80]', 0x1C26, 16),  # strip 1
    ('Layer1VramBuffer[128..144]', 0x1C66, 16),  # strip 2
    ('Layer1VramBuffer[192..208]', 0x1CA6, 16),  # strip 3
]


def main():
    target = int(sys.argv[1]) if len(sys.argv) > 1 else 96
    launch_both()
    r = DebugClient(RECOMP_PORT)
    o = DebugClient(ORACLE_PORT)
    try:
        r.cmd('pause'); o.cmd('pause')
        br = r.cmd('frame').get('frame', 0)
        bo = o.cmd('frame').get('frame', 0)
        r.cmd(f'step {target}'); o.cmd(f'step {target}')
        deadline = time.time() + 60
        while time.time() < deadline:
            rf = r.cmd('frame').get('frame', 0)
            of = o.cmd('frame').get('frame', 0)
            if rf >= br + target and of >= bo + target:
                break
            time.sleep(0.1)
        print(f'[step] recomp at frame={rf}, oracle at frame={of}')
        print(f'{"":32s}  {"recomp":<24s} {"oracle":<24s} match')
        print('-' * 96)
        for name, addr, length in POINTS:
            rb = dump_ram(r, addr, length)
            ob = dump_ram(o, addr, length)
            match = 'OK' if rb == ob else 'DIFF'
            rhex = rb.hex()
            ohex = ob.hex()
            print(f'{name:32s}  {rhex:<24s} {ohex:<24s} {match}')

        # Map16 RAM sanity: pick a location that should have valid block
        # data at the visible region. The first level's map16 block data
        # lives in $7EC800 (for Layer 1). At frame 96 we're past level
        # loading; check first 32 bytes.
        print(f'\n[map16] $7EC800 first 32 bytes (Layer 1 decoded blocks):')
        # $7EC800 = g_ram[0xC800] since g_ram spans $7E0000-$7FFFFF
        rm = dump_ram(r, 0xC800, 32)
        om = dump_ram(o, 0xC800, 32)
        print(f'  recomp: {rm.hex()}')
        print(f'  oracle: {om.hex()}')
        print(f'  {"match" if rm == om else "DIFF"}')
    finally:
        r.close(); o.close()
        _kill()


if __name__ == '__main__':
    main()
