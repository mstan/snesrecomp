"""Bug #8 phase 7: list every WRAM byte emu wrote during the mode 4->5
transition frame (+196), and list every WRAM byte recomp wrote during
its GameMode 4->5 frame (f94). The set-difference (emu-wrote but
recomp-didn't) points at the call path recomp is missing.

Uses emu_wram_delta (snes9x bridge per-frame snapshot diff) on the
emu side and get_wram_trace (Tier 1) on the recomp side.

Usage from repo root:
    python snesrecomp/tests/l3/_probe_bug8_emu_write_delta.py
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


def rb(c, cmd, addr, w=1):
    r = c.cmd(f'{cmd} 0x{addr:x} {w}')
    h = r.get('hex', '').replace(' ', '')
    if not h: return None
    return int.from_bytes(bytes.fromhex(h)[:w], 'little')


def main():
    launch()
    c = DebugClient(RECOMP_PORT)
    try:
        c.cmd('pause')

        # --- EMU: walk to the transition frame. Prior probes ran
        # step_to(c, 100) first, which runs 100 emu frames as a side-effect
        # (emu_oracle_run_frame fires inside the main loop). Reproduce
        # that here so our emu-frame count matches the phase-3 number.
        step_to(c, 100)

        # Step emu to just before the transition (emu total frame 295 ≈
        # +195 post-step-to-100). Walk one frame at a time until we see
        # the $72 flip; that tells us the precise transition frame for
        # this session.
        pre_72 = rb(c, 'emu_read_wram', 0x72)
        target_extra_max = 300
        found_frame = None
        # Get to the GameMode=4 plateau first (per phase-2, around +150).
        c.cmd('emu_step 120')
        print(f'After step_to(100) + emu_step 120:')
        print(f'  emu GameMode=0x{rb(c,"emu_read_wram",0x100):02x}, $72=0x{rb(c,"emu_read_wram",0x72):02x}')

        # Now step one frame at a time, watching for $72 clear.
        for i in range(target_extra_max):
            c.cmd('emu_step 1')
            cur_72 = rb(c, 'emu_read_wram', 0x72)
            if cur_72 != pre_72 and cur_72 == 0x00 and pre_72 == 0x24:
                print(f'  $72 cleared on this single-frame step (offset {i+1})')
                found_frame = i + 1
                break
            if cur_72 == 0x24 and pre_72 == 0:
                pre_72 = 0x24  # Mario-in-air set; keep watching for clear.
        if found_frame is None:
            print('  WARN: did not observe $72 clear within 300 additional frames')
        print(f'After transition: emu GameMode=0x{rb(c,"emu_read_wram",0x100):02x}, $72=0x{rb(c,"emu_read_wram",0x72):02x}')

        r = c.cmd('emu_wram_delta 0 1fff')
        emu_log = r.get('log', [])
        print(f'\nEmu wrote {r.get("count")} bytes in the +196 frame (within $0-$1FFF):')
        emu_written = {int(e['adr'], 16): (int(e['before'], 16), int(e['after'], 16)) for e in emu_log}
        for addr in sorted(emu_written):
            b, a = emu_written[addr]
            print(f'  $0{addr:04x}: 0x{b:02x} -> 0x{a:02x}')

        # --- RECOMP: capture the equivalent via Tier 1 trace over the f93-f102 window ---
        c.cmd('trace_wram_reset')
        c.cmd('trace_wram 0 1fff')       # Track ALL low-WRAM writes.
        step_to(c, 102)
        r = c.cmd('get_wram_trace')
        rec_log = r.get('log', [])
        # Collapse: keep only the LAST write per address (most recent value).
        rec_written = {}
        for e in rec_log:
            try:
                addr = int(e['adr'], 16)
                val = int(e['val'], 16)
                rec_written.setdefault('first', {}).setdefault(addr, val)
                rec_written.setdefault('last', {})[addr] = val
            except Exception:
                pass
        rec_addrs = set(rec_written.get('last', {}).keys())

        # --- Diff ---
        emu_addrs = set(emu_written.keys())
        emu_only  = emu_addrs - rec_addrs
        rec_only  = rec_addrs - emu_addrs
        both      = emu_addrs & rec_addrs

        print(f'\nWrite set sizes:  emu={len(emu_addrs)}  recomp(f93-f102)={len(rec_addrs)}  shared={len(both)}')
        print(f'\nAddresses EMU wrote in +196 frame but RECOMP never touched in f93-f102:')
        for addr in sorted(emu_only):
            b, a = emu_written[addr]
            print(f'  $0{addr:04x}: 0x{b:02x} -> 0x{a:02x}')

        # Highlight $72 specifically.
        print(f'\n$72 on emu: ', emu_written.get(0x72, 'not written in +196 frame'))
        print(f'$72 on recomp (last write in f93-f102): ',
              rec_written.get('last', {}).get(0x72, 'not written'))

        c.cmd('continue')
    finally:
        c.close(); _kill()


if __name__ == '__main__':
    main()
