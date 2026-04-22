"""Tier 3 validation: use the new wram_at_block / wram_first_change /
block_idx_now commands to characterize the recomp cadence around the
GameMode 3->4->5 transition (the source of bug #8's drift).

For the cadence question we need:
  - At what block_idx does GameMode ($100) first take each value during boot?
  - How many blocks does recomp execute per recomp-frame?
  - Compared against emu's per-frame block work (rough proxy: how many
    Tier 1 wram writes per emu frame).
"""
import sys, pathlib, time, subprocess, socket
THIS_DIR = pathlib.Path(__file__).parent
sys.path.insert(0, str(THIS_DIR))
from harness import DebugClient, RECOMP_PORT  # noqa: E402

REPO = pathlib.Path(__file__).parent.parent.parent.parent
ORACLE_EXE = REPO / 'build' / 'bin-x64-Oracle' / 'smw.exe'


def _kill():
    subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'], capture_output=True)


def launch():
    _kill(); time.sleep(0.5)
    subprocess.Popen([str(ORACLE_EXE), '--paused'],
                     cwd=str(REPO),
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    deadline = time.time() + 20
    while time.time() < deadline:
        try:
            s = socket.create_connection(('127.0.0.1', RECOMP_PORT), timeout=0.3)
            s.close(); return
        except OSError:
            time.sleep(0.2)
    raise RuntimeError('no TCP connect')


def step_to(c, target):
    cur = c.cmd('frame').get('frame', 0)
    if cur >= target: return cur
    c.cmd(f'step {target - cur}')
    deadline = time.time() + 60
    while time.time() < deadline:
        if c.cmd('frame').get('frame', 0) >= target: return target
        time.sleep(0.03)


def main():
    launch()
    c = DebugClient(RECOMP_PORT)
    try:
        c.cmd('pause')

        # Arm wide WRAM trace + Tier 3 anchors before stepping.
        c.cmd('trace_wram_reset')
        c.cmd('trace_wram 0 1ffff')
        c.cmd('tier3_anchor_on 1024')

        # Step deep enough to span boot through past mode 5.
        step_to(c, 220)

        # Snapshot: where are we now.
        now = c.cmd('block_idx_now')
        print(f'block_idx_now: {now}')
        astatus = c.cmd('tier3_anchor_status')
        print(f'tier3_anchor_status: {astatus}')

        # Find GameMode transitions by scanning for first writes to $100
        # with each successive value. wram_first_change is per-byte; for
        # the byte $0100 we just look for the first write that touches it.
        # Ranged search.
        print()
        print('Walking GameMode transitions across boot...')
        cur_bi = 0
        seen = []
        for _ in range(20):
            r = c.cmd(f'wram_first_change 100 {cur_bi+1}')
            if not r.get('found'): break
            bi = r.get('bi'); val = r.get('val'); frame = r.get('frame'); func = r.get('func')
            # Reconstruct $100 value AT that block.
            wat = c.cmd(f'wram_at_block {bi} 100 1')
            actual = wat.get('hex', '??')
            print(f'  bi={bi:>8} f{frame:>4} addr=$0100  written {val}  actual_after=0x{actual}  func={func}')
            seen.append((bi, frame, val, actual))
            cur_bi = bi

        # Frames vs blocks ratio.
        if seen:
            first_bi, first_frame, _, _ = seen[0]
            last_bi, last_frame, _, _ = seen[-1]
            df = last_frame - first_frame
            dbi = last_bi - first_bi
            ratio = (dbi / df) if df > 0 else float('inf')
            print()
            print(f'Span: f{first_frame} (bi={first_bi}) -> f{last_frame} (bi={last_bi})')
            print(f'  {dbi} blocks across {df} frames = {ratio:.1f} blocks/frame avg')

        # Also dump WRAM at a few key block_idx points to verify
        # reconstruction works.
        if seen:
            print()
            print('Reconstruction sanity checks (wram_at_block at each transition):')
            for bi, fr, val, actual in seen[:8]:
                # Read $100 + a few neighboring bytes.
                r = c.cmd(f'wram_at_block {bi} 100 4')
                print(f'  bi={bi}: $100..$103 = {r.get("hex")}  '
                      f'(anchor_bi={r.get("anchor_bi")} applied={r.get("applied_writes")})')

        c.cmd('continue')
    finally:
        c.close(); _kill()


if __name__ == '__main__':
    main()
