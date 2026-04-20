"""One-shot: read $7E:005B on recomp and oracle at frame 95 to test
whether UploadLevelLayer1And2Tilemaps branches differently.

The function (src/gen/smw_00_gen.c:1447, ROM $00:87AD) branches on
`g_ram[0x5B] & 1` at the top. Recomp writes 179 VRAM words at frame 95;
oracle writes 909 — a 5× undercount concentrated in $V28xx BG1 tilemap.
If $5B differs between the two sides, the branch divergence explains
the undercount and the root cause is upstream of the function. If $5B
matches, divergence is inside the function (DMA size / mode / VMAIN).

Also prints $5A..$5F for context (this area is display-control state).
"""
import sys
import time
import subprocess
import socket
import pathlib

THIS_DIR = pathlib.Path(__file__).parent
sys.path.insert(0, str(THIS_DIR))

from harness import RECOMP_EXE, ORACLE_EXE, RECOMP_PORT, ORACLE_PORT, DebugClient  # noqa: E402


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


def read_byte(client, addr):
    r = client.cmd(f'read_ram 0x{addr:x} 1')
    hexs = r.get('hex', '')
    return int(hexs.split()[0], 16) if hexs else None


def main():
    target = int(sys.argv[1]) if len(sys.argv) > 1 else 95
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

        print(f'Target frame reached (recomp frame={rf}, oracle frame={of})')
        print(f'WRAM $7E:005A..005F:')
        print(f'{"addr":<6} {"recomp":<8} {"oracle":<8} {"match?":<8}')
        for a in range(0x58, 0x60):
            rv = read_byte(r, a)
            ov = read_byte(o, a)
            match = 'Y' if rv == ov else 'DIFF'
            rv_s = f'0x{rv:02x}' if rv is not None else '?'
            ov_s = f'0x{ov:02x}' if ov is not None else '?'
            print(f'${a:04x}  {rv_s:<8} {ov_s:<8} {match}')

        # Key fact we came for: bit 0 of $5B.
        rv5b = read_byte(r, 0x5B)
        ov5b = read_byte(o, 0x5B)
        print()
        print(f'UploadLevelLayer1And2Tilemaps branch input (bit 0 of $5B):')
        print(f'  recomp: $5B=0x{rv5b:02x} -> bit0={rv5b & 1} -> branch='
              + ('$8849 (long path, more DMAs)' if (rv5b & 1) else '$87C0 (short path, 4 DMAs)'))
        print(f'  oracle: $5B=0x{ov5b:02x} -> bit0={ov5b & 1} -> branch='
              + ('$8849 (long path, more DMAs)' if (ov5b & 1) else '$87C0 (short path, 4 DMAs)'))
        if (rv5b & 1) != (ov5b & 1):
            print()
            print('VERDICT: $5B.bit0 DIFFERS. Branch divergence is the cause.')
            print('Next: trace what writes $5B upstream (likely a PPU-mode or')
            print('display-setting write during level load). The side that')
            print('writes bit0 correctly fires the longer DMA chain.')
        else:
            print()
            print('VERDICT: $5B.bit0 matches. Both sides take the same branch.')
            print('Divergence is INSIDE UploadLevelLayer1And2Tilemaps —')
            print('candidates: DMA size ($4315/$4316), VMAIN ($2115), DMA mode,')
            print('or a clobbered intermediate register across WriteReg calls.')
    finally:
        r.close(); o.close()
        _kill()


if __name__ == '__main__':
    main()
