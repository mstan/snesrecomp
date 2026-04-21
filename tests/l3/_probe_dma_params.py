"""Trace DMA channel-1 parameter writes + $420B triggers on both binaries
during step-to-frame 96. Compare trigger counts and count-reg values.
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


def main():
    target = int(sys.argv[1]) if len(sys.argv) > 1 else 96
    launch_both()
    r = DebugClient(RECOMP_PORT); o = DebugClient(ORACLE_PORT)
    try:
        r.cmd('pause'); o.cmd('pause')
        # Trace DMA ch1 params (0x4310..0x4317) and MDMAEN (0x420B)
        for lo, hi in [(0x4310, 0x4317), (0x420B, 0x420B)]:
            r.cmd(f'trace_reg {lo:x} {hi:x}')
            o.cmd(f'trace_reg {lo:x} {hi:x}')
        br = r.cmd('frame').get('frame', 0)
        bo = o.cmd('frame').get('frame', 0)
        r.cmd(f'step {target}'); o.cmd(f'step {target}')
        deadline = time.time() + 60
        while time.time() < deadline:
            rf = r.cmd('frame').get('frame', 0)
            of = o.cmd('frame').get('frame', 0)
            if rf >= br + target and of >= bo + target: break
            time.sleep(0.1)
        rf = r.cmd('frame').get('frame', 0)
        of = o.cmd('frame').get('frame', 0)
        print(f'[step] target={target} recomp_frame={rf} oracle_frame={of}')
        rtrace = r.cmd('get_reg_trace')
        otrace = o.cmd('get_reg_trace')
        rlog = rtrace.get('log', [])
        olog = otrace.get('log', [])
        print(f'[trace] recomp={len(rlog)} (entries reported={rtrace.get("entries")}) '
              f'oracle={len(olog)} (entries={otrace.get("entries")})')

        def writes_at(log, a):
            return [e for e in log if int(e['adr'], 16) == a]

        # Count $420B triggers at target frame
        def frame_of(e): return e['f']
        target_frame = min(rlog[-1]['f'] if rlog else 0, olog[-1]['f'] if olog else 0)
        # Collect per-frame trigger count (MDMAEN writes)
        for side, log in [('RECOMP', rlog), ('ORACLE', olog)]:
            trigs = writes_at(log, 0x420B)
            by_f = {}
            for t in trigs: by_f.setdefault(t['f'], []).append(int(t['val'], 16))
            print(f'\n=== {side} $420B triggers by frame ===')
            for f in sorted(by_f)[-8:]:
                vals = by_f[f]
                print(f'  f{f}: {len(vals)} triggers, values={[f"0x{v:02x}" for v in vals]}')

        # Show channel-1 count reg ($4315, $4316) writes per frame
        for side, log in [('RECOMP', rlog), ('ORACLE', olog)]:
            print(f'\n=== {side} $4315/$4316 (DMA ch1 count) writes — last frame 10 entries ===')
            cnt_writes = [e for e in log if int(e['adr'], 16) in (0x4315, 0x4316)]
            last_frame = max((e['f'] for e in cnt_writes), default=0)
            for e in cnt_writes:
                if e['f'] >= last_frame - 1:
                    print(f'  f{e["f"]} ${e["adr"]}={e["val"]} func={e["func"]}')

        # Show ALL channel-1 params for the last full frame
        last_f = max(rlog[-1]['f'], olog[-1]['f']) if rlog and olog else 0
        for side, log in [('RECOMP', rlog), ('ORACLE', olog)]:
            print(f'\n=== {side} all ch1 params (0x4310-0x4317) at frame {last_f} ===')
            rel = [e for e in log if e['f'] == last_f and 0x4310 <= int(e['adr'],16) <= 0x4317]
            for e in rel[:40]:
                print(f'  ${e["adr"]}={e["val"]} func={e["func"]}')
            if len(rel) > 40:
                print(f'  ... and {len(rel)-40} more')
    finally:
        r.close(); o.close(); _kill()


if __name__ == '__main__':
    main()
