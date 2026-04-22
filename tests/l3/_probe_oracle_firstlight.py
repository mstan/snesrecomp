"""First-light probe for the embedded snes9x oracle backend.

Launches the Oracle|x64 build of smw.exe (which has snes9x linked in),
drives it to a handful of early frames, and verifies the emu_* TCP
commands are wired end-to-end:

  - emu_list reports snes9x as a compiled-in + active backend
  - emu_is_loaded returns true after init
  - emu_read_wram returns a sane-looking byte
  - emu_cpu_regs returns a plausible 65816 register snapshot
  - recomp's read_ram at the same frame/address for a point comparison

First-light does NOT guarantee WRAM-by-WRAM match (input-layout
parity between the runner's 12-bit-per-player and the bridge's
SNES-hardware layout is a later task). The goal is to prove the
emulator is present, initialized, and queryable.

Usage from repo root:
    python snesrecomp/tests/l3/_probe_oracle_firstlight.py
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
    if not ORACLE_EXE.exists():
        raise RuntimeError(f'Oracle build missing: {ORACLE_EXE}\n'
                           f'Build it with: MSBuild smw.sln -p:Configuration=Oracle -p:Platform=x64')
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


def rb(c, addr, n=1):
    r = c.cmd(f'read_ram 0x{addr:x} {n}')
    b = bytes.fromhex(r.get('hex', '').replace(' ', ''))
    if n == 1:
        return b[0] if b else None
    return int.from_bytes(b[:n], 'little') if b else None


def emu_rb(c, addr, n=1):
    r = c.cmd(f'emu_read_wram 0x{addr:x} {n}')
    if not r.get('ok'):
        return None, r.get('error', '?')
    b = bytes.fromhex(r.get('hex', ''))
    if n == 1:
        return (b[0] if b else None), None
    return (int.from_bytes(b[:n], 'little') if b else None), None


def step_to(c, target):
    cur = c.cmd('frame').get('frame', 0)
    if cur >= target:
        return cur
    c.cmd(f'step {target - cur}')
    deadline = time.time() + 60
    while time.time() < deadline:
        if c.cmd('frame').get('frame', 0) >= target:
            return target
        time.sleep(0.05)


def main():
    launch()
    c = DebugClient(RECOMP_PORT)
    ok = True
    try:
        c.cmd('pause')

        # 1. emu_list — backend registry sanity.
        r = c.cmd('emu_list')
        print(f'emu_list: {r}')
        if not r.get('ok') or 'snes9x' not in r.get('backends', []):
            print('  FAIL: snes9x not in backend registry')
            ok = False
        elif r.get('active') != 'snes9x':
            print(f'  FAIL: active backend = {r.get("active")!r}, expected snes9x')
            ok = False

        # 2. emu_is_loaded — the emu initialized the ROM.
        r = c.cmd('emu_is_loaded')
        print(f'emu_is_loaded: {r}')
        if not r.get('loaded'):
            print('  FAIL: emu backend reports not loaded')
            ok = False

        # 3. emu_cpu_regs at startup (before stepping).
        r = c.cmd('emu_cpu_regs')
        print(f'emu_cpu_regs (pre-step): {r}')
        if not r.get('ok'):
            print('  FAIL: emu_cpu_regs did not respond')
            ok = False

        # 4. Step to a few early frames and compare $72 side-by-side.
        for target in (5, 30, 60, 94):
            step_to(c, target)
            f = c.cmd('frame').get('frame', 0)
            nat = rb(c, 0x72)
            emu, err = emu_rb(c, 0x72)
            print(f'  f{f:>3}: recomp $72 = 0x{nat:02x}  emu $72 = '
                  + (f'0x{emu:02x}' if emu is not None else f'ERR({err})'))
            if emu is None:
                ok = False

        # 5. emu_cpu_regs after stepping — PC should have moved at least once.
        r = c.cmd('emu_cpu_regs')
        print(f'emu_cpu_regs (post-step): {r}')
        if not r.get('ok'):
            ok = False

        # 6. emu_read_wram bounds check — request the end of WRAM.
        r = c.cmd('emu_read_wram 0x1fff0 16')
        print(f'emu_read_wram $1fff0+16: {r}')
        if not r.get('ok'):
            print('  FAIL: end-of-WRAM read rejected')
            ok = False

        print()
        if ok:
            print('FIRST-LIGHT: PASS — snes9x oracle is live and queryable.')
        else:
            print('FIRST-LIGHT: FAIL — see checks above.')

        c.cmd('continue')
    finally:
        c.close(); _kill()


if __name__ == '__main__':
    main()
