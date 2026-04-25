"""Pause-on-write probe for $A9 = 0x7E under WRAM=0x55 fill.

Crash chain: under 0x55 fill, attract demo crashes ~50 frames in at SprStatus08
because $9E+11 = $A9 = 0x7E (Yoshi sprite type) and $14C8+11 = $14D3 = 0x08
(alive). This probe sets a value-predicated watchpoint so execution pauses
the instant the bad value is written, then dumps writer + call stack.

Requires: recomp build with memset(snes->ram, 0x55) at snes.c:69 AND
smw_rtl.c host-side bool I_RESET-gate fix.
"""
import sys, pathlib, time, subprocess, socket
THIS_DIR = pathlib.Path(__file__).parent
sys.path.insert(0, str(THIS_DIR))
from harness import RECOMP_EXE, RECOMP_PORT, DebugClient  # noqa: E402


def _kill():
    subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'], capture_output=True)


def launch():
    _kill(); time.sleep(0.5)
    subprocess.Popen([str(RECOMP_EXE), '--paused'],
        cwd=str(RECOMP_EXE.parent.parent.parent),
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    deadline = time.time() + 15
    while time.time() < deadline:
        try:
            s = socket.create_connection(('127.0.0.1', RECOMP_PORT), timeout=0.3)
            s.close(); return
        except OSError: time.sleep(0.2)
    raise RuntimeError('no connect')


def main():
    launch()
    c = DebugClient(RECOMP_PORT)
    try:
        c.cmd('pause')
        # Two value-predicated watchpoints, one for each suspect byte.
        c.cmd('watch_add a9 7e')
        c.cmd('watch_add 14d3 08')
        # Resume — recomp will park on the watch hit.
        c.cmd('continue')

        # Poll for parked state.
        deadline = time.time() + 30
        parked = None
        while time.time() < deadline:
            try:
                p = c.cmd('parked')
                if p.get('parked'):
                    parked = p
                    break
            except Exception as ex:
                print(f'parked poll failed (process may have crashed): {ex}')
                break
            time.sleep(0.05)

        if parked is None:
            print('No parked state captured within 30s. Process may have crashed.')
            try:
                f = c.cmd('frame')
                print(f'  current frame: {f.get("frame", "?")}')
            except Exception:
                pass
            return

        print('PARKED:')
        for k, v in parked.items():
            print(f'  {k}: {v}')
        # Dump player state + level-prep state at the bad write.
        try:
            f = c.cmd('frame'); print(f'  frame: {f.get("frame", "?")}')
            for adr, name in [(0x71, 'PlayerXLo'), (0x73, 'PlayerYLo'),
                              (0x94, 'PlayerXLo2'), (0x96, 'PlayerYLo2'),
                              (0x100, 'GameMode'), (0x13C6, 'SpawningSprite_idx'),
                              (0x65, 'Layer1Data_lo'), (0x66, 'Layer1Data_mid'), (0x67, 'Layer1Data_hi')]:
                r = c.cmd(f'read_ram {adr:x} 1')
                print(f'  ${adr:04x} {name} = {r.get("data", "?")}')
        except Exception as ex:
            print(f'state dump failed: {ex}')

    finally:
        try: c.close()
        except Exception: pass
        _kill()


if __name__ == '__main__':
    main()
