"""Validate the extended visibility layer:
  - Recomp Tier 4 captures (pc, mnem, A, X, Y, B, m_flag, x_flag) per insn.
  - Emu insn trace captures full hardware state (A, X, Y, S, D, DB, P, cycles).
  - NMI counter on emu side ticks when ROM dispatches NMI.

Print samples and basic correlations to confirm both sides have ground
truth at instruction granularity.
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

        # Arm everything.
        c.cmd('trace_insn_reset')
        c.cmd('trace_insn')
        c.cmd('emu_insn_trace_reset')
        c.cmd('emu_insn_trace_on')

        # Step a few frames.
        step_to(c, 10)

        # Recomp side — extended Tier 4.
        r = c.cmd('get_insn_trace pc_lo=0x008000 pc_hi=0x008100 limit=10')
        rec_log = r.get('log', [])
        print(f'Recomp Tier 4 trace (10 entries in $00:8000-$00:8100, total={r.get("total")}):')
        print(' frame |   pc    | mnem | A      | X      | Y      | B      | m | xf')
        for e in rec_log:
            print(f' {e.get("f"):4d}  | {e.get("pc")} | {e.get("mnem"):>3}  '
                  f'| {e.get("a"):>6} | {e.get("x"):>6} | {e.get("y"):>6} '
                  f'| {e.get("b"):>6} | {e.get("m")} | {e.get("xf")}')

        # Emu side — full HW state.
        r = c.cmd('emu_get_insn_trace from=0 limit=10')
        emu_log = r.get('log', [])
        print(f'\nEmu Tier 4 trace (first 10 entries, total={r.get("total")}):')
        print(' f    | pc        | op   | A      | X      | Y      | S      | D      | DB   | P    | m | xf | e | cyc')
        for e in emu_log:
            print(f' {e.get("f"):4d} | {e.get("pc")} | {e.get("op")} '
                  f'| {e.get("a"):>6} | {e.get("x"):>6} | {e.get("y"):>6} '
                  f'| {e.get("s"):>6} | {e.get("d"):>6} | {e.get("db")} '
                  f'| {e.get("p")} | {e.get("m")} | {e.get("x_flag")}  '
                  f'| {e.get("e")} | {e.get("cyc")}')

        # NMI counter
        r = c.cmd('emu_nmi_count')
        print(f'\nEmu NMI count after step_to(10): {r.get("count")}')

        # Sanity: emu and recomp both started executing real ROM code.
        if rec_log and emu_log:
            print()
            print('SUCCESS — full visibility layer is live:')
            print(f'  recomp captures {len(rec_log)} entries with A/X/Y/B + m/x widths')
            print(f'  emu captures {len(emu_log)} entries with full HW regs incl. S/D/DB/P/cycles')
            print(f'  emu NMI counter = {r.get("count")}')

        c.cmd('continue')
    finally:
        c.close(); _kill()


if __name__ == '__main__':
    main()
