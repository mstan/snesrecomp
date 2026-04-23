"""Frozen koopa step 2: trace SpriteStatus writes on both sides.

The dwell-diff probe revealed recomp puts a koopa in slot 0 (XSpeed=0,
frozen) while oracle puts it in slot 9 (XSpeed=0xf8, walking). The
first $14C8-$14D3 (12 slot statuses) write that differs identifies
the spawn-attribution divergence. Get the writer attribution on
each side."""
from __future__ import annotations
import json, pathlib, socket, subprocess, sys, time

REPO = pathlib.Path(r'F:/Projects/SuperMarioWorldRecomp')
EXE = REPO / 'build/bin-x64-Oracle/smw.exe'
TARGET_MODE = 0x07
ADDR_LO = 0x14C8  # SpriteStatus[0]
ADDR_HI = 0x14D3  # SpriteStatus[11]
MAX_BOOT = 2000


def cmd(s, f, l):
    s.sendall((l + '\n').encode())
    return json.loads(f.readline())


def step1(s, f):
    b = cmd(s, f, 'frame').get('frame', 0)
    cmd(s, f, 'step 1')
    dl = time.time() + 5
    while time.time() < dl:
        if cmd(s, f, 'frame').get('frame', 0) > b: return b + 1
        time.sleep(0.01)


def main():
    subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.5)
    p = subprocess.Popen([str(EXE), '--paused'],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, cwd=str(REPO))
    try:
        s = socket.socket()
        for _ in range(50):
            try: s.connect(('127.0.0.1', 4377)); break
            except (ConnectionRefusedError, OSError): time.sleep(0.2)
        f = s.makefile('r'); f.readline()

        # Arm traces BEFORE any stepping.
        cmd(s, f, 'trace_wram_reset')
        cmd(s, f, f'trace_wram {ADDR_LO:x} {ADDR_HI:x}')
        cmd(s, f, 'emu_wram_trace_reset')
        cmd(s, f, f'emu_wram_trace_add {ADDR_LO:x} {ADDR_HI:x}')

        # Advance recomp to mode 0x07 + dwell 30.
        rframe = None
        for fr in range(1, MAX_BOOT + 1):
            step1(s, f)
            if int(cmd(s, f, 'dump_ram 0x100 1')['hex'].replace(' ', ''), 16) == TARGET_MODE:
                rframe = fr; break
        for _ in range(30):
            step1(s, f)
        # Catch oracle up to its mode 0x07 + 30 dwell.
        while int(cmd(s, f, 'emu_read_wram 0x100 1')['hex'].replace(' ', ''), 16) != TARGET_MODE:
            cmd(s, f, 'emu_step 1')
        for _ in range(30):
            cmd(s, f, 'emu_step 1')

        r = cmd(s, f, 'get_wram_trace')
        rlog = r.get('log', [])
        print(f'recomp SpriteStatus writes ({len(rlog)} total) — first 40 + slot 0/9 only:')
        slot0_writes = [e for e in rlog if int(e.get('adr','0x0'),16) == 0x14C8]
        slot9_writes = [e for e in rlog if int(e.get('adr','0x0'),16) == 0x14D1]
        print(f'  slot 0 ($14C8) writes: {len(slot0_writes)}')
        for e in slot0_writes[:20]:
            print(f'    f{e.get("f"):4} {e.get("old","?")}->{e.get("val")} '
                  f'func={e.get("func")} parent={e.get("parent")}')
        print(f'  slot 9 ($14D1) writes: {len(slot9_writes)}')
        for e in slot9_writes[:20]:
            print(f'    f{e.get("f"):4} {e.get("old","?")}->{e.get("val")} '
                  f'func={e.get("func")} parent={e.get("parent")}')

        r = cmd(s, f, 'emu_get_wram_trace')
        elog = r.get('log', [])
        slot0_e = [e for e in elog if int(e.get('adr','0x0'),16) == 0x14C8]
        slot9_e = [e for e in elog if int(e.get('adr','0x0'),16) == 0x14D1]
        print(f'\noracle SpriteStatus writes ({len(elog)} total):')
        print(f'  slot 0 ($14C8) writes: {len(slot0_e)}')
        for e in slot0_e[:20]:
            print(f'    f{e.get("f"):4} pc={e.get("pc")} '
                  f'{e.get("before")}->{e.get("after")} bank={e.get("bank_src")}')
        print(f'  slot 9 ($14D1) writes: {len(slot9_e)}')
        for e in slot9_e[:20]:
            print(f'    f{e.get("f"):4} pc={e.get("pc")} '
                  f'{e.get("before")}->{e.get("after")} bank={e.get("bank_src")}')
        return 0
    finally:
        try: s.close()
        except Exception: pass
        p.terminate()
        try: p.wait(timeout=5)
        except subprocess.TimeoutExpired: p.kill()
        subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


if __name__ == '__main__':
    sys.exit(main())
