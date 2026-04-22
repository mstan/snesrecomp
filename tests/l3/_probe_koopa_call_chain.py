"""Bug: koopa spawn missing on recomp. Trace which functions in the
sprite-load call chain actually fire on recomp during the title demo.

Chain (per SMWDisX): GameMode14_InLevel -> GameMode14_InLevel_Bank02
  -> ParseLevelSpriteList ($02:A7FC) -> CODE_02A93C ($02:A93C, where
  STA SpriteStatus,X fires).

If ParseLevelSpriteList is called but CODE_02A93C-region blocks aren't
entered, the function takes an early-exit. Otherwise the function
itself isn't reached.
"""
import sys, pathlib, time, subprocess, socket
THIS_DIR = pathlib.Path(__file__).parent
sys.path.insert(0, str(THIS_DIR))
from harness import DebugClient, RECOMP_PORT  # noqa: E402

REPO = pathlib.Path(__file__).parent.parent.parent.parent
ORACLE_EXE = REPO / 'build' / 'bin-x64-Oracle' / 'smw.exe'


def _kill(): subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'], capture_output=True)


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
        c.cmd('trace_calls_reset')
        c.cmd('trace_calls')
        c.cmd('trace_blocks_reset')
        c.cmd('trace_blocks')
        step_to(c, 300)

        # Q1: was ParseLevelSpriteList called?
        r = c.cmd('get_call_trace contains=ParseLevelSpriteList')
        calls = r.get('log', [])
        print(f'ParseLevelSpriteList calls during run: {len(calls)}')
        for e in calls[:5]:
            print(f"  f{e.get('f')} d{e.get('depth')} {e.get('func')} <- {e.get('parent')}")

        # Q2: was GameMode14_InLevel_Bank02 called?
        r = c.cmd('get_call_trace contains=GameMode14_InLevel_Bank02')
        calls = r.get('log', [])
        print(f'\nGameMode14_InLevel_Bank02 calls: {len(calls)}')
        for e in calls[:3]:
            print(f"  f{e.get('f')} d{e.get('depth')} {e.get('func')} <- {e.get('parent')}")

        # Q3: which blocks within $02:A7FC-$02:A9F8 did recomp enter?
        r = c.cmd('get_block_trace pc_lo=0x02a7fc pc_hi=0x02a9f8')
        blocks = r.get('log', [])
        print(f'\nRecomp block entries in $02:A7FC-$02:A9F8 (sprite-spawn function range): {len(blocks)}')
        # Show distinct PCs.
        pcs = {}
        for b in blocks:
            pc = b.get('pc'); pcs[pc] = pcs.get(pc, 0) + 1
        for pc in sorted(pcs.keys(), key=lambda s: int(s,16) if isinstance(s,str) else 0):
            print(f"  {pc} x{pcs[pc]}")

        # Q4: read trace on the key gate variables — TrueFrame ($14),
        # SpriteDataPtr ($CE-$D0), Layer1XPos ($1A-$1B). If recomp's
        # ParseLevelSpriteList sees TrueFrame & 1 == 1 always, it exits
        # at line 5226 (Return02A84B) before doing anything.
        c.cmd('trace_wram_reads_reset')
        c.cmd('trace_wram_reads 14 14')   # TrueFrame
        c.cmd('trace_wram_reads ce d0')   # SpriteDataPtr
        c.cmd('step 50')

        # Brief pause to let writes flush.
        import time as _t; _t.sleep(0.3)
        r = c.cmd('get_wram_read_trace')
        rlog = r.get('log', [])
        print(f'\nReads of TrueFrame/$14 + SpriteDataPtr/$CE-$D0 during step: {len(rlog)}')
        # Show the reads from inside ParseLevelSpriteList (filter by func name).
        psl_reads = [e for e in rlog if 'ParseLevelSpriteList' in e.get('func', '')
                     or 'ParseLevelSpriteList' in e.get('parent', '')]
        print(f'  Of which from ParseLevelSpriteList: {len(psl_reads)}')
        for e in psl_reads[:8]:
            print(f"    f{e.get('f')} {e.get('adr')}={e.get('val')} w={e.get('w')} "
                  f"{e.get('func')}")

        c.cmd('continue')
    finally:
        c.close(); _kill()


if __name__ == '__main__':
    main()
