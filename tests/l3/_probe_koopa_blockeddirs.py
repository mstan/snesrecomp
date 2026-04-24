"""Koopa-falls step 4: compare writes to SpriteBlockedDirs[9] ($1591)
on recomp vs oracle. Oracle has bit 2 set (on-ground). Recomp reads 0
at IsOnGround. Find what writes $1591 and when.

SpriteBlockedDirs is $1588,X (slot 9 = $1591)."""
from __future__ import annotations
import json, pathlib, socket, subprocess, sys, time

REPO = pathlib.Path(r'F:/Projects/SuperMarioWorldRecomp')
EXE = REPO / 'build/bin-x64-Oracle/smw.exe'
TARGET_MODE = 0x07
ADDRS = {
    0x1591: 'SpriteBlockedDirs[9]',
    0x00E1: 'SpriteYPosLo[9]',
    0x14DD: 'SpriteYPosHi[9]',
    0x00B3: 'SpriteYSpeed[9]',
    # Oracle's walking koopa is actually in slot 9 per insn trace
    # (x=0x0009 at $018B4F). Same slot, so diff = pure physics difference.
}


def cmd(s, f, l):
    s.sendall((l + '\n').encode()); return json.loads(f.readline())


def step1(s, f):
    b = cmd(s, f, 'frame').get('frame', 0)
    cmd(s, f, 'step 1')
    dl = time.time() + 5
    while time.time() < dl:
        if cmd(s, f, 'frame').get('frame', 0) > b: return b + 1
        time.sleep(0.01)


def e_byte(s, f, a):
    r = cmd(s, f, f'emu_read_wram 0x{a:x} 1')
    return int(r['hex'].replace(' ', ''), 16) if r.get('ok') else -1


def r_byte(s, f, a):
    r = cmd(s, f, f'dump_ram 0x{a:x} 1')
    return int(r['hex'].replace(' ', ''), 16)


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

        cmd(s, f, 'trace_wram_reset')
        cmd(s, f, 'emu_wram_trace_reset')
        for a in ADDRS:
            cmd(s, f, f'trace_wram {a:x} {a:x}')
            cmd(s, f, f'emu_wram_trace_add {a:x} {a:x}')

        # Advance recomp to mode 0x07 + dwell 40.
        for fr in range(1, 2000):
            step1(s, f)
            if r_byte(s, f, 0x100) == TARGET_MODE: break
        for _ in range(40): step1(s, f)
        # Catch oracle up.
        while e_byte(s, f, 0x100) != TARGET_MODE:
            cmd(s, f, 'emu_step 1')
        for _ in range(40): cmd(s, f, 'emu_step 1')

        print(f'\n== recomp frame={cmd(s, f, "frame").get("frame")}, '
              f'state snapshot ==')
        for a, lbl in ADDRS.items():
            print(f'  {lbl:25s} ${a:04x} = 0x{r_byte(s, f, a):02x}')

        print(f'\n== oracle state snapshot ==')
        for a, lbl in ADDRS.items():
            print(f'  {lbl:25s} ${a:04x} = 0x{e_byte(s, f, a):02x}')

        print('\n== recomp writes to $1591 only ==')
        r = cmd(s, f, 'get_wram_trace')
        for e in r.get('log', []):
            if int(e.get('adr', '0x0'), 16) == 0x1591:
                print(f'  f{e.get("f"):4} adr={e.get("adr")} '
                      f'{e.get("old","?")}->{e.get("val")} func={e.get("func")} '
                      f'parent={e.get("parent")}')

        print('\n== oracle writes ==')
        r = cmd(s, f, 'emu_get_wram_trace')
        for e in r.get('log', [])[:40]:
            pc = e.get('pc', '?')
            print(f'  f{e.get("f"):4} adr={e.get("adr")} '
                  f'{e.get("before","?")}->{e.get("after")} pc={pc} '
                  f'bank={e.get("bank_src","?")}')
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
