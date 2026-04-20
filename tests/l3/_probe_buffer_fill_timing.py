"""Check when $7E:7D00 first diverges between recomp and oracle.

Dumps the first 32 bytes of $7E:7D00 at frames 1, 5, 10, 20, 50, 80, 90, 95, 96.
Tells us WHEN the exanim tile buffer first gets populated with bad data.
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
    r = client.cmd(f'read_ram 0x{addr:x} {length}')
    return bytes.fromhex(r.get('hex', ''))


def step_to(client, target):
    base = client.cmd('frame').get('frame', 0)
    remaining = target - base
    if remaining <= 0:
        return
    client.cmd(f'step {remaining}')
    deadline = time.time() + 30
    while time.time() < deadline:
        if client.cmd('frame').get('frame', 0) >= target:
            return
        time.sleep(0.1)
    raise RuntimeError(f'stuck at {client.cmd("frame")}')


def main():
    frames = [60, 61, 62, 63, 64, 65, 66]
    launch_both()
    r = DebugClient(RECOMP_PORT)
    o = DebugClient(ORACLE_PORT)
    try:
        r.cmd('pause'); o.cmd('pause')
        br = r.cmd('frame').get('frame', 0)
        bo = o.cmd('frame').get('frame', 0)
        print(f'[init] baselines r={br} o={bo}\n')
        for f in frames:
            step_to(r, br + f)
            step_to(o, bo + f)
            rb = dump(r, 0x7D00, 32)
            ob = dump(o, 0x7D00, 32)
            match = 'OK' if rb == ob else 'DIFF'
            rz = sum(1 for b in rb if b == 0)
            oz = sum(1 for b in ob if b == 0)
            print(f'frame {f:3d}: {match} | recomp zeros={rz}/32 oracle zeros={oz}/32')
            if match == 'DIFF':
                print(f'  recomp: {rb.hex()}')
                print(f'  oracle: {ob.hex()}')
    finally:
        r.close(); o.close()
        _kill()


if __name__ == '__main__':
    main()
