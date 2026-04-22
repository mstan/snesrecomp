"""Bug #8 phase 10: full $72 write history on both sides, to see the
actual timeline difference.

Prior probe showed recomp DOES enter CODE_00EF60 (the STZ PlayerInAir
block) 35 times over 300 frames. So the bug is NOT a missing call.
This probe captures:
  - Every $72 write on recomp (Tier 1) across 300 frames
  - Every $72 write on emu (bridge write-hook) across the matching
    emu-frame range
And prints them side-by-side.

Usage:
    python snesrecomp/tests/l3/_probe_bug8_full_72_history.py
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

        # Arm both traces BEFORE stepping so we capture writes from frame 1.
        c.cmd('trace_wram_reset')
        c.cmd('trace_wram 72 72')
        c.cmd('emu_wram_trace_reset')
        c.cmd('emu_wram_trace_add 72 72')

        step_to(c, 300)

        # Recomp side.
        r = c.cmd('get_wram_trace')
        rec_log = r.get('log', [])
        print(f'Recomp writes to $72 (f1-f300), total={len(rec_log)}:')
        print(' frame | val  | width | func')
        for e in rec_log:
            print(f' {e.get("f"):4d}  | {e.get("val")} |   {e.get("w")}   | {e.get("func")} <- {e.get("parent")}')

        # Emu side.
        r = c.cmd('emu_get_wram_trace')
        emu_log = r.get('log', [])
        print(f'\nEmu writes to $72 (emu frames 1-~400), total={len(emu_log)}:')
        print(' frame |    PC      | before -> after | bank')
        for e in emu_log:
            f = e.get('f'); pc = e.get('pc')
            before = e.get('before'); after = e.get('after')
            bank = e.get('bank_src')
            print(f' {f:4}  | {pc:10} | {before} -> {after}     | {bank}')

        # Summarize: what values does $72 hold over time per side.
        print(f'\nFinal $72: recomp=0x{int.from_bytes(bytes.fromhex(c.cmd("read_ram 0x72 1").get("hex","").replace(" ","")[:2]),"little"):02x}  '
              f'emu=0x{int.from_bytes(bytes.fromhex(c.cmd("emu_read_wram 0x72 1").get("hex",""))[:1],"little"):02x}')

        c.cmd('continue')
    finally:
        c.close(); _kill()


if __name__ == '__main__':
    main()
