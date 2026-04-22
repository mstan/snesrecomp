"""Bug #8 phase 4: dump the GM12PrepLevel branch-deciding WRAM vars
on both sides and diff them.

SMWDisX shows GM12PrepLevel has a branch that determines whether $72
is set to 0x24 (CODE_00A6CC) or 0x00 (CODE_00A6B6 -> STZ PlayerInAir).
The branch depends on:
  ObjectTileset            ($01931, 1 byte)  — must be < $10, else $24
  DATA_00A625[ObjectTileset] — ROM table; after LSR, must be != 0 for $00 path
  ShowMarioStart           ($0141D, 1 byte)  — must be 0 for $00 path
  SublevelCount            ($0141A, 1 byte)  — must be 0 for $00 path
  DisableNoYoshiIntro      ($0141F, 1 byte)  — must be 0 for $00 path
  SkipMidwayCastleIntro    ($013CF, 1 byte)  — must be 0 for $00 path
  CarryYoshiThruLvls       ($00DC1, 1 byte)  — affects upstream branch

Also sampling LevelEntranceType ($0192A) and KeyholeTimer ($01434) for
context. If any of these differ between recomp and emu at the moment
GM12PrepLevel runs, that's the bug.

Strategy: drive recomp to f100 (just past mode-4 transition, $72=0x24
locked in) and emu to +196 (just past mode-4 transition, $72=0x00).
Dump gating vars on both sides.

Usage from repo root:
    python snesrecomp/tests/l3/_probe_bug8_branch_vars.py
"""
import sys, pathlib, time, subprocess, socket
THIS_DIR = pathlib.Path(__file__).parent
sys.path.insert(0, str(THIS_DIR))
from harness import DebugClient, RECOMP_PORT  # noqa: E402

REPO = pathlib.Path(__file__).parent.parent.parent.parent
ORACLE_EXE = REPO / 'build' / 'bin-x64-Oracle' / 'smw.exe'

VARS = [
    ('ObjectTileset',         0x1931, 1),
    ('ShowMarioStart',        0x141D, 1),
    ('SublevelCount',         0x141A, 1),
    ('DisableNoYoshiIntro',   0x141F, 1),
    ('SkipMidwayCastleIntro', 0x13CF, 1),
    ('CarryYoshiThruLvls',    0x0DC1, 1),
    ('LevelEntranceType',     0x192A, 1),
    ('KeyholeTimer',          0x1434, 1),
    ('GameMode',              0x0100, 1),
    ('PlayerInAir ($72)',     0x0072, 1),
]


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


def rb(c, cmd, addr, width=1):
    r = c.cmd(f'{cmd} 0x{addr:x} {width}')
    hex_s = r.get('hex', '').replace(' ', '')
    if not hex_s: return None
    b = bytes.fromhex(hex_s)
    return int.from_bytes(b[:width], 'little') if b else None


def step_to(c, target):
    cur = c.cmd('frame').get('frame', 0)
    if cur >= target: return cur
    c.cmd(f'step {target - cur}')
    deadline = time.time() + 60
    while time.time() < deadline:
        if c.cmd('frame').get('frame', 0) >= target: return target
        time.sleep(0.03)


def dump_vars(c, cmd):
    out = {}
    for label, addr, width in VARS:
        out[label] = rb(c, cmd, addr, width)
    return out


def fmt(v, w):
    return '??' if v is None else f'0x{v:0{w*2}x}'


def main():
    launch()
    c = DebugClient(RECOMP_PORT)
    try:
        c.cmd('pause')

        # Put recomp just past GameMode-4 transition.
        step_to(c, 100)
        # Walk emu to just past its GameMode-4 transition (+196 per phase 3).
        c.cmd('emu_step 196')

        rec = dump_vars(c, 'read_ram')
        emu = dump_vars(c, 'emu_read_wram')

        print(f'{"variable":<26}  {"recomp":>8}   {"emu":>8}   diff')
        print('-' * 64)
        diverged = 0
        for label, addr, width in VARS:
            r = rec[label]; e = emu[label]
            mark = '  ***' if r != e else ''
            if r != e: diverged += 1
            print(f'{label:<26}  {fmt(r,width):>8}   {fmt(e,width):>8}   {mark}')

        print()
        # Evaluate the GM12PrepLevel branch predicate on each side:
        #   takes STZ path iff ObjectTileset<$10 AND
        #                      DATA_00A625[ObjectTileset] after LSR != 0 AND
        #                      ShowMarioStart|SublevelCount|DisableNoYoshiIntro == 0 AND
        #                      SkipMidwayCastleIntro == 0
        # We don't have the ROM table here; we report the WRAM-only part.
        def predicate_note(side, vs):
            ot = vs['ObjectTileset']; sms = vs['ShowMarioStart']
            sc = vs['SublevelCount']; dni = vs['DisableNoYoshiIntro']
            smci = vs['SkipMidwayCastleIntro']
            wram_ok = (ot is not None and ot < 0x10 and
                       (sms or 0) == 0 and (sc or 0) == 0 and
                       (dni or 0) == 0 and (smci or 0) == 0)
            return f'{side}: ObjectTileset<$10={ot is not None and ot<0x10}, ' \
                   f'ShowMario|Sublvl|NoYoshi=0 -> {((sms or 0)|(sc or 0)|(dni or 0))==0}, ' \
                   f'SkipMidway=0 -> {(smci or 0)==0}  | WRAM predicate favors STZ path: {wram_ok}'

        print(predicate_note('recomp', rec))
        print(predicate_note('emu   ', emu))
        print()
        print(f'Diverged variables: {diverged}')
        if rec["PlayerInAir ($72)"] == 0x24 and emu["PlayerInAir ($72)"] == 0x00:
            print('*** Bug #8 manifests exactly as expected at this sync point. ***')

        c.cmd('continue')
    finally:
        c.close(); _kill()


if __name__ == '__main__':
    main()
