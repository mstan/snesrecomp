"""Bug #8 phase 9: does recomp ever reach $EF60 (the STZ PlayerInAir block)?

We know emu clears $72 at PC=$00:$EF6B via CODE_00EF60, which lives
inside recomp function RunPlayerBlockCode_00EEE1 (range $EEE1-$EFBC).

If recomp ever enters that function and reaches the STZ, we'd expect
to see the parent of $72's write show up as RunPlayerBlockCode_00EEE1.
Our trace showed only InitializeLevelRAM writing $72, never EEE1. So
either:
  (A) recomp never calls RunPlayerBlockCode_00EEE1, OR
  (B) recomp calls it but takes a branch that skips past $EF60.

This probe uses Tier 2 block trace to enumerate which blocks inside
$EEE1-$EFBC recomp actually enters during a wide frame window.
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

        # Arm Tier 2 block trace, filtered to the EEE1-EFBC range on bank 00.
        c.cmd('trace_blocks_reset')
        r = c.cmd('trace_blocks')
        print(f'trace_blocks: {r}')

        # Step recomp over a wide window to catch any entry into the function.
        step_to(c, 300)

        r = c.cmd(f'get_block_trace pc_lo=0xee00 pc_hi=0xf000')
        log = r.get('log', [])
        print(f'\nBlocks entered in $00:EE00-$00:F000 (count={len(log)}):')
        pc_set = set()
        for e in log:
            pc = e.get('pc')
            pc_set.add(pc)
        for pc in sorted(pc_set, key=lambda s: int(s, 16) if isinstance(s, str) else 0):
            # Count occurrences for each PC.
            count = sum(1 for e in log if e.get('pc') == pc)
            print(f'  {pc}  x{count}')

        ef60_entered = any(
            (isinstance(e.get('pc'), str) and 0x00ef40 <= int(e['pc'], 16) <= 0x00ef70)
            for e in log)
        print(f'\nDid recomp enter any PC in $EF40-$EF70 (near the STZ)?  {ef60_entered}')

        # Also check if the enclosing function was ever CALLED.
        r = c.cmd('trace_calls_reset')
        r = c.cmd('trace_calls')
        print(f'trace_calls: {r}')
        c.cmd('step 50')   # advance 50 more frames to catch a fresh call
        r = c.cmd('get_call_trace contains RunPlayerBlockCode_00EEE1')
        call_log = r.get('log', [])
        print(f'\nRunPlayerBlockCode_00EEE1 calls observed: {len(call_log)}')
        for e in call_log[:5]:
            print(f'  f{e.get("f")}: {e.get("func")} <- {e.get("parent")}')

        c.cmd('continue')
    finally:
        c.close(); _kill()


if __name__ == '__main__':
    main()
