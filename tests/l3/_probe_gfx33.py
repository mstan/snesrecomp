"""Probe Gfx33 DMA source data to determine if the UploadLevelExAnimationData
bug is a bad input (upstream) or bad DMA (downstream).

Dumps:
  - $0D76-$0D81: Gfx33Src/DestAddrA/B/C pointers
  - First 128 bytes at Gfx33SrcC (the source the DMA reads)
  - Same for SrcB, SrcA

At attract-mode frame 96.
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


def dump(client, addr, length):
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
        print(f'[step] recomp at {rf}, oracle at {of}\n')

        # Pointer block $0D76-$0D81 (12 bytes)
        rpt = dump(r, 0x0D76, 12)
        opt = dump(o, 0x0D76, 12)
        names = ['SrcA', 'SrcB', 'SrcC', 'DestA', 'DestB', 'DestC']
        print(f'Gfx33 pointer block (2 bytes each):')
        for i, n in enumerate(names):
            rv = rpt[i*2] | (rpt[i*2+1] << 8)
            ov = opt[i*2] | (opt[i*2+1] << 8)
            mark = 'OK' if rv == ov else 'DIFF'
            print(f'  {n:6s} = recomp:0x{rv:04x}  oracle:0x{ov:04x}  {mark}')

        # For each non-zero source pointer, dump the first 128 bytes it
        # points at. Source addresses are in bank $7E (set via STY #$7E).
        for i, n in enumerate(('SrcA', 'SrcB', 'SrcC')):
            rv = rpt[i*2] | (rpt[i*2+1] << 8)
            ov = opt[i*2] | (opt[i*2+1] << 8)
            if rv == 0 and ov == 0:
                continue
            # Source bank is 0x7E per STY #$7E setup — bank 0x7E = low
            # 64KB of g_ram, so read directly from g_ram[rv] for 128 bytes.
            rd = dump(r, rv, 128)
            od = dump(o, ov, 128)
            matches = sum(1 for i in range(128) if rd[i] == od[i])
            print(f'\n{n} source @ $7E:{rv:04x} (recomp) / $7E:{ov:04x} (oracle), 128 bytes')
            print(f'  {matches}/128 bytes match')
            # First 32 bytes hex for inspection
            print(f'  recomp[0..31]: {rd[:32].hex()}')
            print(f'  oracle[0..31]: {od[:32].hex()}')
            # Count zeros
            rz = sum(1 for b in rd if b == 0)
            oz = sum(1 for b in od if b == 0)
            print(f'  zero bytes: recomp={rz}/128, oracle={oz}/128')
    finally:
        r.close(); o.close()
        _kill()


if __name__ == '__main__':
    main()
