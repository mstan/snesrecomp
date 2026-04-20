"""Trace every VRAM write in frame 96 on both recomp and oracle
(pure interpreter via --theirs), find writes where they diverge.

Frame 96 is where attract-mode level-load happens and recomp's VRAM
diverges from the interpreter by ~1100 words (per _attract_vram_diff.py).
Tracing at the write level gives us function attribution on the recomp
side, so we can scope which generated/HLE routine is responsible.
"""
import pathlib
import socket
import subprocess
import sys
import time
import json

THIS_DIR = pathlib.Path(__file__).parent
sys.path.insert(0, str(THIS_DIR))
from harness import RECOMP_EXE, ORACLE_EXE, RECOMP_PORT, ORACLE_PORT, DebugClient  # noqa: E402


def _kill():
    subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'],
                   capture_output=True, check=False)


def launch():
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
    time.sleep(2.0)


def wait_step_done(client, target):
    deadline = time.time() + 60
    while time.time() < deadline:
        f = client.cmd('frame').get('frame', 0)
        if f >= target:
            return f
        time.sleep(0.1)
    raise RuntimeError('step timeout')


def main():
    launch()
    r = DebugClient(RECOMP_PORT)
    o = DebugClient(ORACLE_PORT)
    try:
        r.cmd('pause')
        o.cmd('pause')
        # Step to frame 95 (pre-divergence)
        r.cmd('step 95')
        o.cmd('step 95')
        wait_step_done(r, 95)
        wait_step_done(o, 95)
        # Turn on trace for the divergent regions
        r.cmd('trace_vram_reset')
        o.cmd('trace_vram_reset')
        for rng in ['0x0000 0x0fff', '0x2000 0x2fff', '0x6000 0x6fff']:
            r.cmd(f'trace_vram {rng}')
            o.cmd(f'trace_vram {rng}')
        # Step 1 more frame
        r.cmd('step 1')
        o.cmd('step 1')
        wait_step_done(r, 96)
        wait_step_done(o, 96)
        rt = r.cmd('get_vram_trace')
        ot = o.cmd('get_vram_trace')
        r_log = rt.get('log', [])
        o_log = ot.get('log', [])
        print(f'[trace] recomp entries={len(r_log)}, oracle entries={len(o_log)}')
        # Histograms by function
        from collections import Counter
        r_by_func = Counter(e.get('func', '?') for e in r_log)
        o_by_func = Counter(e.get('func', '?') for e in o_log)
        print('[recomp] top VRAM-writing functions (frame 96):')
        for func, n in r_by_func.most_common(10):
            print(f'  {n:5d}  {func}')
        print('[oracle] top VRAM-writing functions (frame 96):')
        for func, n in o_by_func.most_common(10):
            print(f'  {n:5d}  {func}')
        # Collapse paired lo/hi writes: group by adr, take FINAL word value
        def collapse(log):
            final = {}
            order = []
            for e in log:
                adr = int(e.get('adr', '0x0'), 16)
                val = int(e.get('val', '0x0'), 16)
                if adr not in final:
                    order.append(adr)
                final[adr] = (val, e.get('func', '?'))
            return [(a, *final[a]) for a in order]
        r_writes = collapse(r_log)
        o_writes = collapse(o_log)
        print(f'[collapse] unique-addr writes: recomp={len(r_writes)}, oracle={len(o_writes)}')
        # Address-set diff
        r_addrs = set(a for a, _, _ in r_writes)
        o_addrs = set(a for a, _, _ in o_writes)
        only_recomp = r_addrs - o_addrs
        only_oracle = o_addrs - r_addrs
        shared = r_addrs & o_addrs
        print(f'[addr-set] shared={len(shared)}, only-recomp={len(only_recomp)}, only-oracle={len(only_oracle)}')
        # Among shared: how many have same final value?
        r_map = {a: v for a, v, _ in r_writes}
        o_map = {a: v for a, v, _ in o_writes}
        same_val = sum(1 for a in shared if r_map[a] == o_map[a])
        diff_val = len(shared) - same_val
        print(f'[shared] same-val={same_val}, diff-val={diff_val}')
        # Show first 8 only-oracle writes (writes the recomp is missing)
        print()
        print('[only-oracle] first 15 writes missing from recomp side:')
        for a in sorted(only_oracle)[:15]:
            print(f'  $V{a:04x} = 0x{o_map[a]:04x}')
        print()
        print('[only-recomp] first 15 writes missing from oracle side:')
        for a in sorted(only_recomp)[:15]:
            val, func = r_map[a], [f for aa, _, f in r_writes if aa == a][0]
            print(f'  $V{a:04x} = 0x{val:04x}  ({func})')
    finally:
        r.close()
        o.close()
        _kill()


if __name__ == '__main__':
    main()
