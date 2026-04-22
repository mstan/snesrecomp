"""Bug #8 phase 15: dump recomp's tracked A/X/Y at every entry to
RunPlayerBlockCode_00EEE1, using the new block-hook register
capture. Compare to emu's Y at the equivalent moment.

The block hook now records (frame, depth, pc, a, x, y, func) for
every basic-block entry. Probes can pull any frame's block trace
filtered to a PC of interest and inspect the registers.
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

        c.cmd('trace_blocks_reset')
        c.cmd('trace_blocks')

        # Step deep into the gravity-accumulation window.
        step_to(c, 215)

        # Pull every block-trace entry at $EEE1 (function entry).
        r = c.cmd('get_block_trace pc_lo=0xeee1 pc_hi=0xeee1')
        log = r.get('log', [])
        print(f'Block-trace entries at $00:$EEE1 over f1-f215: {len(log)}')
        print(' frame | depth | A      | X      | Y      | func')
        for e in log[:40]:
            print(f' {e.get("f"):4d}  |   {e.get("d"):2}  | {e.get("a"):>6} | {e.get("x"):>6} | {e.get("y"):>6} | {e.get("func")}')

        # Compare to emu's Y at the same PC moment. Emu's CPU regs
        # snapshot is whatever it is right now (post-frame at NMI), so
        # not a clean compare. But we can step emu 1 frame, then read
        # emu's last write to $7D from the new wram trace and infer A.
        print()
        c.cmd('emu_wram_trace_reset')
        c.cmd('emu_wram_trace_add 7d 7d')
        # Run a few emu frames in the gravity-stable window so EF60 fires.
        c.cmd('emu_step 5')
        r = c.cmd('emu_get_wram_trace')
        emu_writes = r.get('log', [])
        print(f'Emu $7D writes (next 5 emu frames): {len(emu_writes)}')
        ef60_writes = [e for e in emu_writes if e.get('pc') == '0x00ef62']
        print(f'  Of which from $EF60 (STA $7D): {len(ef60_writes)}')
        for e in ef60_writes:
            print(f'    emu_f{e.get("f")} PC={e.get("pc")} write {e.get("before")} -> {e.get("after")}')
        if ef60_writes:
            after_vals = set(e.get('after') for e in ef60_writes)
            print(f'\n  Emu A at $EF60 STA (what gets stored to $7D) = {sorted(after_vals)}')

        # Extract recomp's most-recent A at $EEE1 (proxy for what flows to $EF60)
        if log:
            last = log[-1]
            print(f'\nRecomp A at last $EEE1 entry (f{last.get("f")}): {last.get("a")}')
            print(f'Recomp X at last $EEE1 entry: {last.get("x")}')
            print(f'Recomp Y at last $EEE1 entry: {last.get("y")}')

        c.cmd('continue')
    finally:
        c.close(); _kill()


if __name__ == '__main__':
    main()
