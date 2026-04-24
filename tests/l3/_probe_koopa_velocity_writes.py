"""Koopa-falls step 1: trace SpriteYSpeed[9] ($00B3) and
SpriteXSpeed[9] ($00BF) writes on both sides. Also SpriteYPosLo[9]
($00E1) and SpriteYPosHi[9] ($14DD) to see when/why Y drifts.

Seed candidates:
  - SpriteXSpeed[9] never written on recomp → init path missed
  - SpriteYSpeed[9] = 0x40 on recomp vs 0 on oracle → gravity vs. grounded
  - SpriteYPosLo[9] = 0xaf on recomp vs 0x60 on oracle → Y diverged early

Find the FIRST write that differs on each address."""
from __future__ import annotations
import json, pathlib, socket, subprocess, sys, time

REPO = pathlib.Path(r'F:/Projects/SuperMarioWorldRecomp')
EXE = REPO / 'build/bin-x64-Oracle/smw.exe'
TARGET_MODE = 0x07
MAX_BOOT = 2000

# Addresses of interest (slot 9).
ADDRS = {
    0x00B3: 'SpriteYSpeed[9]',
    0x00BF: 'SpriteXSpeed[9]',
    0x00E1: 'SpriteYPosLo[9]',
    0x14DD: 'SpriteYPosHi[9]',
    0x00ED: 'SpriteXPosLo[9]',
    0x14F1: 'SpriteXPosHi[9]',
    0x14D1: 'SpriteStatus[9]',
    0x14E9: 'SpriteNumber[9]',
}
ADDR_LO = min(a for a, _ in ADDRS.items())   # 0x00B3 — need multiple ranges


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

        # Arm traces on each address of interest.
        cmd(s, f, 'trace_wram_reset')
        cmd(s, f, 'emu_wram_trace_reset')
        for addr in ADDRS:
            cmd(s, f, f'trace_wram {addr:x} {addr:x}')
            cmd(s, f, f'emu_wram_trace_add {addr:x} {addr:x}')

        # Advance recomp to mode 0x07 + dwell 35 to capture spawn + first
        # physics frames.
        for fr in range(1, MAX_BOOT + 1):
            step1(s, f)
            if int(cmd(s, f, 'dump_ram 0x100 1')['hex'].replace(' ', ''), 16) == TARGET_MODE:
                break
        for _ in range(35): step1(s, f)
        # Catch oracle up.
        while int(cmd(s, f, 'emu_read_wram 0x100 1')['hex'].replace(' ', ''), 16) != TARGET_MODE:
            cmd(s, f, 'emu_step 1')
        for _ in range(35): cmd(s, f, 'emu_step 1')

        # Pull recomp writes, grouped by address.
        r = cmd(s, f, 'get_wram_trace')
        rlog = r.get('log', [])
        print(f'recomp writes ({len(rlog)} total):')
        for addr, label in ADDRS.items():
            hits = [e for e in rlog if int(e.get('adr', '0x0'), 16) == addr]
            print(f'\n  {label} (${addr:04x}): {len(hits)} writes')
            for e in hits[:20]:
                print(f'    f{e.get("f"):4} {e.get("old","?")}->{e.get("val")} '
                      f'func={e.get("func")} parent={e.get("parent")}')

        r = cmd(s, f, 'emu_get_wram_trace')
        elog = r.get('log', [])
        print(f'\noracle writes ({len(elog)} total):')
        for addr, label in ADDRS.items():
            hits = [e for e in elog if int(e.get('adr', '0x0'), 16) == addr]
            print(f'\n  {label} (${addr:04x}): {len(hits)} writes')
            for e in hits[:20]:
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
