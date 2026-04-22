"""Bug #8 phase 8: emu-side PC attribution for writes to $72.

Now that snes9x's write bus is hooked via s9x_write_hook, every write
to $72 on the emu side gets logged with the PC of the writing
instruction. This is the missing piece for bug #8: it names the
ROM-level function that clears $72 on real-hardware-equivalent emu
but is (apparently) missing or misgenerated in recomp.

Usage from repo root:
    python snesrecomp/tests/l3/_probe_bug8_emu_pc_attribution.py
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

        # Step recomp to f100 (this also runs 100 emu frames via the
        # main-loop side effect, so total emu frames = 100 at this point).
        step_to(c, 100)

        # Arm watchpoint: log every write to $72 on the emu side.
        r = c.cmd('emu_wram_trace_reset')
        print(f'emu_wram_trace_reset: {r}')
        r = c.cmd('emu_wram_trace_add 72 72')
        print(f'emu_wram_trace_add 0x72: {r}')

        # Step emu across the transition. From prior data the transition
        # happens around total_emu_frame ~296, so 300 extra frames from 100
        # is a safe over-shoot.
        print('\nStepping emu +300 frames to cross the GameMode 4->5 transition...')
        c.cmd('emu_step 300')

        # Read current state.
        print(f'After: emu GameMode=0x{rb(c,"emu_read_wram",0x100):02x}  $72=0x{rb(c,"emu_read_wram",0x72):02x}')

        r = c.cmd('emu_get_wram_trace')
        log = r.get('log', [])
        print(f'\nWrites to $72 observed on emu (count={r.get("count")}):')
        print(' frame |    PC      | before -> after | bank_src')
        print('-------+------------+------------------+--------')
        for e in log:
            f = e.get('f'); pc = e.get('pc')
            before = e.get('before'); after = e.get('after')
            bank = e.get('bank_src')
            print(f'  {f!s:4} | {pc!s:10} |   {before} -> {after}   |  {bank}')

        # For any "clear" write (0x24 -> 0x00), highlight it.
        clears = [e for e in log if e.get('after') == '0x00' and e.get('before') == '0x24']
        print(f'\nWrites that cleared $72 from 0x24 -> 0x00: {len(clears)}')
        for e in clears:
            pc = e['pc']
            print(f'  At frame {e["f"]}, PC={pc}  (bank {e["bank_src"]})')
            print(f'  --> Cross-reference this PC in SMWDisX to identify the ROM function.')

        c.cmd('continue')
    finally:
        c.close(); _kill()


if __name__ == '__main__':
    main()
