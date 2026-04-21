"""Run both binaries to frame 96 tracing $V2800-$V2FFF, sort writes by time
(emission index = order captured), and print the first N writes from each
side side-by-side so we can see the first temporal divergence and the call
stack at that point.

Oracle emits func=(none) and stack=[] because oracle has no recomp funcs;
that's fine — the key is what OUR stack looks like at the first
disagreement.
"""
import sys, pathlib, time, subprocess, socket
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


def main():
    target = int(sys.argv[1]) if len(sys.argv) > 1 else 96
    lo = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x2800
    hi = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x2fff
    launch_both()
    r = DebugClient(RECOMP_PORT); o = DebugClient(ORACLE_PORT)
    try:
        r.cmd('pause'); o.cmd('pause')
        r.cmd(f'trace_vram {lo:x} {hi:x}')
        o.cmd(f'trace_vram {lo:x} {hi:x}')
        br = r.cmd('frame').get('frame', 0)
        bo = o.cmd('frame').get('frame', 0)
        r.cmd(f'step {target}'); o.cmd(f'step {target}')
        deadline = time.time() + 60
        while time.time() < deadline:
            rf = r.cmd('frame').get('frame', 0)
            of = o.cmd('frame').get('frame', 0)
            if rf >= br + target and of >= bo + target: break
            time.sleep(0.1)
        rlog = r.cmd('get_vram_trace').get('log', [])
        olog = o.cmd('get_vram_trace').get('log', [])
        print(f'[trace] recomp={len(rlog)} oracle={len(olog)} in ${lo:04x}-${hi:04x}')

        # First N writes from each, in capture order (which == time order)
        print('\n=== FIRST 15 WRITES (temporal order) ===')
        print('idx  recomp                         | oracle')
        for i in range(max(15, 0)):
            ri = rlog[i] if i < len(rlog) else None
            oi = olog[i] if i < len(olog) else None
            rs = f'f{ri["f"]} ${ri["adr"]}={ri["val"]} ({ri["func"]})' if ri else '(end)'
            os_ = f'f{oi["f"]} ${oi["adr"]}={oi["val"]}' if oi else '(end)'
            mark = ' ' if ri and oi and ri['adr']==oi['adr'] and ri['val']==oi['val'] else '*'
            print(f'{i:3d}{mark} {rs:45s}| {os_}')

        # Dump full stack at FIRST recomp write and first oracle-only write
        print('\n=== CALL STACK — recomp write #0 ===')
        if rlog:
            w = rlog[0]
            print(f'  addr=${w["adr"]} val={w["val"]} func={w["func"]}')
            for s in w.get('stack', []): print(f'    {s}')

        # Identify first oracle write whose (adr, val) is NOT in recomp's set
        rec_set = set((w['adr'], w['val']) for w in rlog)
        for i, w in enumerate(olog):
            key = (w['adr'], w['val'])
            if key not in rec_set:
                print(f'\n=== FIRST ORACLE-ONLY WRITE (oracle idx {i}) ===')
                print(f'  addr=${w["adr"]} val={w["val"]} frame={w["f"]}')
                # What was recomp doing at this temporal position?
                if i < len(rlog):
                    rw = rlog[i]
                    print(f'  recomp at same idx: ${rw["adr"]}={rw["val"]} func={rw["func"]}')
                    for s in rw.get('stack', []): print(f'    {s}')
                else:
                    print(f'  recomp had only {len(rlog)} writes — none at this idx')
                break

        # Last recomp write — did recomp terminate the loop early?
        if rlog:
            w = rlog[-1]
            print(f'\n=== LAST RECOMP WRITE ===')
            print(f'  idx={len(rlog)-1} addr=${w["adr"]} val={w["val"]} func={w["func"]}')
            for s in w.get('stack', []): print(f'    {s}')
    finally:
        r.close(); o.close(); _kill()


if __name__ == '__main__':
    main()
