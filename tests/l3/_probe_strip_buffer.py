"""Probe Layer1VramBuffer at WRAM $1BE6 at attract-mode frame 96.

Tests the hypothesis from the VRAM diff: recomp VRAM has correct low bytes
and zero high bytes across all Layer 1 tilemap writes. If the DMA itself is
fine, the upstream strip-build code must be writing only low bytes into
$1BE6-$1CE5 (32 tile-words * 4 strips = 256 bytes).

Dumps that 256-byte range from recomp and oracle, attributes the divergence
pattern (even indexes = tile-word low, odd indexes = tile-word high).
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


def launch_both_interp():
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


def main():
    target = int(sys.argv[1]) if len(sys.argv) > 1 else 96
    launch_both_interp()
    r = DebugClient(RECOMP_PORT)
    o = DebugClient(ORACLE_PORT)
    try:
        r.cmd('pause')
        o.cmd('pause')
        br = r.cmd('frame').get('frame', 0)
        bo = o.cmd('frame').get('frame', 0)
        print(f'[init] recomp baseline={br}, oracle baseline={bo}')
        r.cmd(f'step {target}')
        o.cmd(f'step {target}')
        deadline = time.time() + 60
        while time.time() < deadline:
            rf = r.cmd('frame').get('frame', 0)
            of = o.cmd('frame').get('frame', 0)
            if rf >= br + target and of >= bo + target:
                break
            time.sleep(0.1)
        print(f'[step] recomp at {rf}, oracle at {of}')

        # Layer1VramBuffer = $1BE6, 256 bytes (4 strips * 64 bytes).
        rv = dump_ram(r, 0x1BE6, 256)
        ov = dump_ram(o, 0x1BE6, 256)
        print(f'\n[dump] Layer1VramBuffer @ WRAM $1BE6 (256 bytes)')

        diffs = [(i, rv[i], ov[i]) for i in range(256) if rv[i] != ov[i]]
        print(f'[diff] {len(diffs)}/256 bytes differ')

        even_diffs = [d for d in diffs if d[0] % 2 == 0]
        odd_diffs = [d for d in diffs if d[0] % 2 == 1]
        recomp_zero_even = sum(1 for _, rb, _ in even_diffs if rb == 0)
        recomp_zero_odd = sum(1 for _, rb, _ in odd_diffs if rb == 0)
        print(f'[pattern] even-index diffs: {len(even_diffs)} (recomp=0 in {recomp_zero_even})')
        print(f'[pattern] odd-index diffs:  {len(odd_diffs)} (recomp=0 in {recomp_zero_odd})')

        # Show first few diffs
        print(f'\n[first 16 diffs]')
        for i, rb, ob in diffs[:16]:
            mark = 'EVEN' if i % 2 == 0 else 'ODD '
            print(f'  [{mark}] $1BE6+{i:03d}=${0x1BE6+i:04x}: recomp=0x{rb:02x} oracle=0x{ob:02x}')

        # If odd indexes are all zero in recomp, that's the smoking gun.
        if len(odd_diffs) > 0 and recomp_zero_odd == len(odd_diffs):
            print(f'\n[VERDICT] odd-index bytes (tile-word HIGH bytes) are '
                  f'all ZERO in recomp — strip-build code is writing low '
                  f'bytes only. DMA itself is innocent.')
        elif len(even_diffs) == 0 and len(odd_diffs) == 0:
            print(f'\n[VERDICT] buffer matches — divergence is downstream '
                  f'(DMA or PPU), not upstream.')
        else:
            print(f'\n[VERDICT] mixed pattern — need deeper tracing.')
    finally:
        r.close()
        o.close()
        _kill()


if __name__ == '__main__':
    main()
