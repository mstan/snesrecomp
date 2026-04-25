"""Trace what code runs on the koopa sprite immediately after Mario
stomps it (f269 contact). Determine which SprStatus handler the koopa
runs at f270+, and what writes to its $14C8+slot status byte.

Goal: empirically establish what dispatch the koopa actually takes
post-stomp, then compare to what SMWDisX expects for a normal Mario
overhead-stomp (should transition to status=09 = Stunned-in-shell).
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


def step_to(c, target):
    base = c.cmd('frame').get('frame', 0)
    if base >= target: return base
    c.cmd(f'step {target - base}')
    deadline = time.time() + 60
    while time.time() < deadline:
        cur = c.cmd('frame').get('frame', 0)
        if cur >= target: return cur
        time.sleep(0.05)
    return -1


def main():
    launch()
    c = DebugClient(RECOMP_PORT)
    try:
        c.cmd('pause')
        # Step to just before contact (f268), arm both call trace and
        # WRAM trace on the koopa's status + sprite-type slots.
        step_to(c, 268)
        c.cmd('trace_calls_reset')
        c.cmd('trace_calls')
        c.cmd('trace_wram_reset')
        # Koopa is sprite slot ? — find by reading $14C8+0..11 (status)
        # at f268. Whichever slot has status=8 is the live sprite.
        for k in range(12):
            r = c.cmd(f'read_ram {0x14c8 + k:x} 1')
            v = r.get('hex', '?').strip()
            r2 = c.cmd(f'read_ram {0x9e + k:x} 1')
            t = r2.get('hex', '?').strip()
            print(f'  slot {k}: status=${v} type=${t}')
        # Trace the entire $14C8 status array AND sprite-misc bytes.
        # Slot 9 specific: status=$14D1, type=$A7
        c.cmd('trace_wram 14d1 14d1')
        c.cmd('trace_wram a7 a7')
        # Step through contact + 20 frames after.
        step_to(c, 290)

        # Pull all SprStatus*/SprXXX_Generic/Spr0to13/SpawnContact calls
        for substr in ['SprStatus09', 'SprStatus02', 'SprStatus04', 'SprStatus03',
                       'SprXXX_Generic_NakedKoopa', 'KillSprite',
                       'SetAsStunned', 'SpawnSprite', 'CreateBounce']:
            r = c.cmd(f'get_call_trace contains={substr} from=269 to=290')
            log = r.get('log', [])
            if log:
                print(f'\n--- {substr}: {len(log)} hits f269-274 ---')
                for e in log[:25]:
                    print(f'  f{e["f"]:4} d{e["d"]:3} {e["func"]:50} parent={e["parent"]}')

        # Pull WRAM writes to $14C8+slot (status changes)
        wt = c.cmd('get_wram_trace')
        wlog = wt.get('log', [])
        status_writes = [e for e in wlog if int(e['adr'], 16) // 12 * 12 in (0x14c4, 0x14c8, 0x14cc, 0x14d0)
                         and 0x14c8 <= int(e['adr'], 16) <= 0x14d3]
        type_writes = [e for e in wlog if 0x9e <= int(e['adr'], 16) <= 0xa9]
        print(f'\n--- writes to $14C8-$14D3 (sprite status) f269-274 ---')
        for e in status_writes:
            print(f'  f{e["f"]:4} ${int(e["adr"], 16):04x} = {e["val"]} ({e["func"]})')
        print(f'\n--- writes to $9E-$A9 (sprite type) f269-274 ---')
        for e in type_writes:
            print(f'  f{e["f"]:4} ${int(e["adr"], 16):04x} = {e["val"]} ({e["func"]})')
    finally:
        try: c.close()
        except Exception: pass
        _kill()


if __name__ == '__main__':
    main()
