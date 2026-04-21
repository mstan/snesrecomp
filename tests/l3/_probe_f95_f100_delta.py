"""For each side, dump WRAM at f95 and at f100, compute the per-side
DELTA SET = {addrs that changed}. Then:

  oracle_only = oracle_delta - recomp_delta
  recomp_only = recomp_delta - oracle_delta

oracle_only contains addresses that oracle wrote between f95 and f100
that recomp did NOT write. The function that clears PlayerInAir in
oracle should appear in oracle_only along with its other side-effects.
"""
import sys, pathlib, time, subprocess, socket
THIS_DIR = pathlib.Path(__file__).parent
sys.path.insert(0, str(THIS_DIR))
from harness import RECOMP_EXE, ORACLE_EXE, RECOMP_PORT, ORACLE_PORT, DebugClient  # noqa: E402


def _kill(): subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'], capture_output=True)


def _ports_ready():
    for p in (RECOMP_PORT, ORACLE_PORT):
        try:
            s = socket.create_connection(('127.0.0.1', p), timeout=0.3); s.close()
        except OSError: return False
    return True


def launch_both():
    _kill(); time.sleep(0.5)
    subprocess.Popen([str(RECOMP_EXE), '--paused'],
        cwd=str(RECOMP_EXE.parent.parent.parent),
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.Popen([str(ORACLE_EXE), '--paused', '--theirs'],
        cwd=str(ORACLE_EXE.parent.parent.parent),
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    deadline = time.time() + 15
    while time.time() < deadline:
        if _ports_ready(): time.sleep(0.3); return
        time.sleep(0.2)


def step_to(c, target):
    base = c.cmd('frame').get('frame', 0)
    if base >= target: return base
    c.cmd(f'step {target - base}')
    deadline = time.time() + 60
    while time.time() < deadline:
        if c.cmd('frame').get('frame', 0) >= target: return target
        time.sleep(0.05)


def rb(c, addr, n):
    r = c.cmd(f'read_ram 0x{addr:x} {n}')
    return bytes.fromhex(r.get('hex', '').replace(' ', ''))


def deltas(snap_a, snap_b):
    """Return dict {addr: (a_val, b_val)} for differing bytes."""
    out = {}
    for i in range(min(len(snap_a), len(snap_b))):
        if snap_a[i] != snap_b[i]:
            out[i] = (snap_a[i], snap_b[i])
    return out


def main():
    launch_both()
    r = DebugClient(RECOMP_PORT); o = DebugClient(ORACLE_PORT)
    try:
        r.cmd('pause'); o.cmd('pause')
        N = 0x2000
        step_to(r, 95); step_to(o, 95)
        r95 = rb(r, 0, N); o95 = rb(o, 0, N)
        step_to(r, 100); step_to(o, 100)
        r100 = rb(r, 0, N); o100 = rb(o, 0, N)
        rdelta = deltas(r95, r100)   # what recomp wrote f95->f100
        odelta = deltas(o95, o100)   # what oracle wrote f95->f100
        ronly = set(rdelta) - set(odelta)
        oonly = set(odelta) - set(rdelta)
        both = set(rdelta) & set(odelta)
        print(f'recomp wrote {len(rdelta)} addrs; oracle wrote {len(odelta)} addrs')
        print(f'addresses only oracle wrote (oracle did, recomp did NOT): {len(oonly)}')
        print(f'addresses only recomp wrote: {len(ronly)}')
        print(f'addresses both wrote: {len(both)}')
        print('\n=== Oracle-only writes (top 60) ===')
        for a in sorted(oonly)[:60]:
            ov95, ov100 = odelta[a]
            print(f'  ${a:04x}  oracle: 0x{ov95:02x} -> 0x{ov100:02x}  (recomp stayed at 0x{r100[a]:02x})')
        print('\n=== Recomp-only writes (top 30, just for asymmetry view) ===')
        for a in sorted(ronly)[:30]:
            rv95, rv100 = rdelta[a]
            print(f'  ${a:04x}  recomp: 0x{rv95:02x} -> 0x{rv100:02x}  (oracle stayed at 0x{o100[a]:02x})')
        # Sanity: $72 should be in oonly with 0x24 -> 0x00.
        if 0x72 in oonly:
            print(f'\n[confirm] $72 in oracle-only delta: {odelta[0x72]}')
    finally:
        r.close(); o.close(); _kill()


if __name__ == '__main__':
    main()
