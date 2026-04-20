"""Trace VRAM writes to a divergent region on both recomp and oracle,
step to frame 96, diff the logs. Attributes each write to the function
that emitted it (g_last_recomp_func at the time of the write).

Starts with a narrow range around the first observed diff ($V0490) to
keep trace log size manageable. Expand if the narrow range is empty.
"""
import sys
import pathlib
import time
import subprocess
import socket
import json

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


def main():
    target = int(sys.argv[1]) if len(sys.argv) > 1 else 96
    # VRAM word range to trace — capture the region where the diff first appears.
    lo = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x0490
    hi = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x0500
    launch_both()
    r = DebugClient(RECOMP_PORT)
    o = DebugClient(ORACLE_PORT)
    try:
        r.cmd('pause'); o.cmd('pause')
        # Enable trace BEFORE stepping
        r.cmd(f'trace_vram {lo:x} {hi:x}')
        o.cmd(f'trace_vram {lo:x} {hi:x}')
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
        print(f'[step] recomp at {rf}, oracle at {of}')

        rtrace = r.cmd('get_vram_trace')
        otrace = o.cmd('get_vram_trace')
        rlog = rtrace.get('log', [])
        olog = otrace.get('log', [])
        print(f'[trace] recomp: {len(rlog)} writes to $V{lo:04x}-$V{hi:04x}')
        print(f'[trace] oracle: {len(olog)} writes to $V{lo:04x}-$V{hi:04x}')

        # Group by address — show first/last values and frames
        def summarize(log, label):
            print(f'\n=== {label} ===')
            if not log:
                print('  (no writes)'); return
            by_addr = {}
            for ent in log:
                adr = int(ent['adr'], 16)
                by_addr.setdefault(adr, []).append(ent)
            for adr in sorted(by_addr):
                writes = by_addr[adr]
                first = writes[0]
                last = writes[-1]
                print(f'  $V{adr:04x}: {len(writes)} writes')
                print(f'    first: frame={first["f"]} val={first["val"]} func={first["func"]}')
                if len(writes) > 1:
                    print(f'    last:  frame={last["f"]} val={last["val"]} func={last["func"]}')
        summarize(rlog, 'RECOMP')
        summarize(olog, 'ORACLE')

        # Cross-sectional at same address: first/last differences
        print('\n=== PER-ADDRESS DIFF (final value) ===')
        r_last = {}
        for ent in rlog:
            r_last[int(ent['adr'], 16)] = ent
        o_last = {}
        for ent in olog:
            o_last[int(ent['adr'], 16)] = ent
        all_addrs = sorted(set(r_last) | set(o_last))
        diffs = 0
        for adr in all_addrs:
            re_ = r_last.get(adr)
            oe = o_last.get(adr)
            r_val = re_['val'] if re_ else None
            o_val = oe['val'] if oe else None
            if r_val != o_val:
                diffs += 1
                rf_ = f'frame={re_["f"]} val={r_val} func={re_["func"]}' if re_ else '(recomp never wrote)'
                of_ = f'frame={oe["f"]} val={o_val} func={oe["func"]}' if oe else '(oracle never wrote)'
                print(f'  $V{adr:04x}:')
                print(f'    recomp: {rf_}')
                print(f'    oracle: {of_}')
        print(f'\n[summary] {diffs} addresses diverge in final value')
    finally:
        r.close(); o.close()
        _kill()


if __name__ == '__main__':
    main()
