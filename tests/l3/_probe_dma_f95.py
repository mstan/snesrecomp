"""Step to frame 94, reset DMA reg trace, then step 2 more frames. Only
frame 95 DMA activity is captured. Compare recomp vs oracle.
"""
import sys, pathlib, time, subprocess, socket
THIS_DIR = pathlib.Path(__file__).parent
sys.path.insert(0, str(THIS_DIR))
from harness import RECOMP_EXE, ORACLE_EXE, RECOMP_PORT, ORACLE_PORT, DebugClient  # noqa: E402


def _kill():
    subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'],
                   capture_output=True, check=False)


def _ports_ready():
    for port in (RECOMP_PORT, ORACLE_PORT):
        try:
            s = socket.create_connection(('127.0.0.1', port), timeout=0.3); s.close()
        except OSError:
            return False
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
    raise RuntimeError('timeout')


def step_to(client, target):
    base = client.cmd('frame').get('frame', 0)
    client.cmd(f'step {target - base}')
    deadline = time.time() + 60
    while time.time() < deadline:
        f = client.cmd('frame').get('frame', 0)
        if f >= target: return f
        time.sleep(0.1)
    return client.cmd('frame').get('frame', 0)


def main():
    launch_both()
    r = DebugClient(RECOMP_PORT); o = DebugClient(ORACLE_PORT)
    try:
        r.cmd('pause'); o.cmd('pause')
        rf = step_to(r, 94); of = step_to(o, 94)
        print(f'[pre-trace] recomp@{rf} oracle@{of}')
        # Reset + start fresh trace
        r.cmd('trace_reg_reset'); o.cmd('trace_reg_reset')
        r.cmd('trace_reg 4300 4317'); o.cmd('trace_reg 4300 4317')
        r.cmd('trace_reg 420b 420b'); o.cmd('trace_reg 420b 420b')
        r.cmd('trace_reg 2115 2117'); o.cmd('trace_reg 2115 2117')
        rf = step_to(r, 96); of = step_to(o, 96)
        print(f'[post-step] recomp@{rf} oracle@{of}')
        rt = r.cmd('get_reg_trace nostack'); ot = o.cmd('get_reg_trace nostack')
        rlog = rt.get('log', []); olog = ot.get('log', [])
        print(f'recomp entries={rt.get("entries")} log={len(rlog)}')
        print(f'oracle entries={ot.get("entries")} log={len(olog)}')

        # Print sequential DMA ops: each DMA = run of $2115/$2116/$2117,
        # then $4310..$4316 setup, then $420B trigger.
        def dump(side, log, max_lines=300):
            print(f'\n=== {side} trace (frame,addr,val,func) — {len(log)} entries ===')
            for i, e in enumerate(log):
                if i >= max_lines:
                    print(f'  ... {len(log)-max_lines} more'); break
                print(f'  f{e["f"]} ${e["adr"]}={e["val"]:<6} {e["func"]}')
        dump('RECOMP f95 activity', [e for e in rlog if e['f']==95], 100000)
        dump('ORACLE f95 activity', [e for e in olog if e['f']==95], 100000)
        dump('RECOMP f96 activity', [e for e in rlog if e['f']==96], 100000)
        dump('ORACLE f96 activity', [e for e in olog if e['f']==96], 100000)
    finally:
        r.close(); o.close(); _kill()


if __name__ == '__main__':
    main()
