"""Tier 4 validation: arm trace_insn, step a few frames, sanity-check
that the per-instruction trace captures plausible entries with valid
PCs and mnemonics.
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

        # Pull the mnemonic table first.
        r = c.cmd('get_insn_mnemonics')
        table = r.get('table', [])
        print(f'Mnemonic table count={len(table)}, first 10: {table[:10]}')
        assert table[0] == '?', 'index 0 should be unknown sentinel'
        assert 'STZ' in table, 'STZ missing from mnemonic table'

        # Arm trace_insn and step.
        c.cmd('trace_insn_reset')
        r = c.cmd('trace_insn')
        print(f'trace_insn: {r}')

        step_to(c, 95)

        # Get a slice — just a small window in the EE-EF range during
        # demo physics. Fall back to wider PC if no entries match.
        r = c.cmd('get_insn_trace pc_lo=0xa600 pc_hi=0xa700 limit=20')
        log = r.get('log', [])
        print(f'\nTier 4 trace in $00:A600-$00:A700, total trace size={r.get("total")}, '
              f'matched={r.get("emitted")}, sample:')
        for e in log[:15]:
            mnid = e.get('m', 0)
            mnem = table[mnid] if mnid < len(table) else '?'
            print(f'  f{e.get("f"):4} bi={e.get("bi"):>8} {e.get("pc")} {mnem}')

        # Verify the trace actually grew (we stepped 95 frames; each
        # frame fires thousands of insns).
        assert (r.get('total') or 0) > 1000, f'trace looks empty: {r}'
        print(f'\nTrace populated: {r.get("total")} insn entries captured.')

        # Quick query: how many INC instructions in the GameMode-advance
        # window? (Look for any INC at $100 indirect via trace.)
        # The pc filter alone won't hit specific opcodes; cheaper to
        # just count by mnemonic via raw scan.
        r = c.cmd('get_insn_trace limit=4096')
        log = r.get('log', [])
        inc_id = table.index('INC')
        inc_count = sum(1 for e in log if e.get('m') == inc_id)
        print(f'INC opcodes in last 4096 captured insns: {inc_count}')

        c.cmd('continue')
    finally:
        c.close(); _kill()


if __name__ == '__main__':
    main()
