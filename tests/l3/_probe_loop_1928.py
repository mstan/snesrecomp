"""One-shot a/b decider for the 5× BG1-tilemap write undercount.

The outer loop in InitializeLevelLayer1And2Tilemaps ($05:809E,
src/gen/smw_05_gen.c:191) runs 32 iterations:
  - initializes g_ram[0x1928] = 0
  - each iter: Buffer...Init × 2, UploadLevelLayer1And2Tilemaps, then
    g_ram[0x1928]++; loop while (g_ram[0x1928] - 0x20) != 0
  - exits with g_ram[0x1928] == 0x20

Per frame 95 trace_vram_diverge: recomp 179 writes, oracle 909 writes
to $V2800-$V2FFF (≈5.08× ratio). Two hypotheses:
  (a) Recomp's outer loop exits early — $1928 < 0x20 at function exit
  (b) Loop runs fully — $1928 == 0x20 on both — divergence is in
      per-iteration DMA effective size / mode / VMAIN

At frame 95 (post-NMI, after auto_00_816A and its upload chain),
$1928 should be 0x20 on any side that completed the loop.

Also reads $1925 (dispatch index into kDispatch_88f5[]) and $1BE4/$1BE5
(Layer1VramAddr) for extra context.
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
        rf = br; of_ = bo
        while time.time() < deadline:
            rf = r.cmd('frame').get('frame', 0)
            of_ = o.cmd('frame').get('frame', 0)
            if rf >= br + target and of_ >= bo + target:
                break
            time.sleep(0.1)
        print(f'Target frame reached (recomp={rf}, oracle={of_})')
        print()

        # Loop counter + dispatch inputs + Layer1VramAddr
        addrs = [
            (0x1925, 'dispatch idx into kDispatch_88f5[]'),
            (0x1928, 'InitLevelLayer1And2Tilemaps outer loop counter'),
            (0x1BE4, 'Layer1VramAddr lo (g_ram[0x1be4])'),
            (0x1BE5, 'Layer1VramAddr hi (g_ram[0x1be5])'),
        ]
        print(f'{"addr":<8} {"recomp":<8} {"oracle":<8} {"match":<8} label')
        for a, label in addrs:
            rv = read_byte(r, a)
            ov = read_byte(o, a)
            match = 'Y' if rv == ov else 'DIFF'
            rv_s = f'0x{rv:02x}' if rv is not None else '?'
            ov_s = f'0x{ov:02x}' if ov is not None else '?'
            print(f'${a:04x}    {rv_s:<8} {ov_s:<8} {match:<8} {label}')

        r1928 = read_byte(r, 0x1928)
        o1928 = read_byte(o, 0x1928)
        print()
        print('=== VERDICT ===')
        if r1928 == 0x20 and o1928 == 0x20:
            print(f'$1928 = 0x20 on BOTH sides → loop completed 32 iterations on both.')
            print('Case (b): divergence is INSIDE each iteration.')
            print('Next: instrument recomp_execute_dma_channel to log')
            print('size/mode/dest per $420B trigger at frame 95, diff count &')
            print('per-transfer size between recomp and oracle.')
        elif r1928 != 0x20 and o1928 == 0x20:
            print(f'$1928 = 0x{r1928:02x} recomp, 0x20 oracle → recomp loop EXITED EARLY.')
            print(f'Case (a): recomp ran {r1928}/32 iterations.')
            print('Expected writes ratio: {:.2f}× -- actual: 5.08×'.format(32/max(r1928, 1)))
            print('Next: find what clobbers $1928 mid-loop or what break condition fires early.')
        elif r1928 == 0x20 and o1928 != 0x20:
            print(f'Unexpected: oracle $1928=0x{o1928:02x} not 0x20. Oracle loop incomplete?')
        else:
            print(f'Both incomplete: recomp=0x{r1928:02x} oracle=0x{o1928:02x}.')
            print('Something more fundamental diverges — check frame advancement.')
    finally:
        r.close(); o.close()
        _kill()


if __name__ == '__main__':
    main()
