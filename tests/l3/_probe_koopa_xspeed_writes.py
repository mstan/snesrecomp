"""Trace writes to PlayerXSpeed ($7B) and Controller_1 ($16) during
the first frame of GM=0x07. Mario's xspeed=1 (recomp) vs xspeed=3
(oracle) is the earliest game-state divergence; tracking the writers
identifies the divergent function (and the divergent input that
drove it).
"""
from __future__ import annotations
import json, pathlib, socket, subprocess, time

REPO = pathlib.Path(r'F:/Projects/SuperMarioWorldRecomp')
EXE = REPO / 'build/bin-x64-Oracle/smw.exe'


def cmd(s, f, line):
    s.sendall((line + '\n').encode())
    return json.loads(f.readline())


def main():
    subprocess.run(['taskkill','/F','/IM','smw.exe'],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.5)
    p = subprocess.Popen([str(EXE),'--paused'], cwd=REPO,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        s = socket.socket()
        for _ in range(50):
            try: s.connect(('127.0.0.1', 4377)); break
            except (ConnectionRefusedError, OSError): time.sleep(0.2)
        f = s.makefile('r'); f.readline()

        for _ in range(2000):
            cmd(s, f, 'step 1')
            if int(cmd(s, f, 'dump_ram 0x100 1')['hex'].replace(' ',''),16) == 7: break
        for _ in range(2000):
            cmd(s, f, 'emu_step 1')
            if int(cmd(s, f, 'emu_read_wram 0x100 1')['hex'].replace(' ',''),16) == 7: break

        # Watch $7B (PlayerXSpeed), $16 (Controller_1), $94 (PlayerXPosNext),
        # $13E0 (PlayerPose).
        cmd(s, f, 'trace_wram_reset')
        cmd(s, f, 'emu_wram_trace_reset')
        for a in (0x7A, 0x7B, 0x16, 0x18, 0x94, 0x13E0):
            cmd(s, f, f'trace_wram {a:x} {a:x}')
            cmd(s, f, f'emu_wram_trace_add {a:x} {a:x}')

        cmd(s, f, 'step 1')
        cmd(s, f, 'emu_step 1')

        rt = cmd(s, f, 'get_wram_trace').get('log', [])
        ot = cmd(s, f, 'emu_get_wram_trace').get('log', [])

        def label(adr):
            adr = int(adr, 16)
            return {0x7A:'XSpeed',0x7B:'XSpeedSub',0x16:'Ctrl_1',0x18:'Ctrl_2',
                    0x94:'XPosNext',0x13E0:'PlayerPose'}.get(adr, f'${adr:x}')

        print(f'=== recomp writes during 1st frame of GM=0x07 ({len(rt)}) ===')
        for e in rt[-40:]:
            print(f'  {label(e["adr"]):11s} {e["adr"]} {e["old"]}->{e["val"]} '
                  f'func={e.get("func", "?"):60s} parent={e.get("parent", "?")}')
        print(f'\n=== oracle writes ({len(ot)}) ===')
        for e in ot[-40:]:
            print(f'  {label(e["adr"]):11s} {e["adr"]} {e.get("before","?")}->{e["after"]} '
                  f'pc={e.get("pc","?")} bank={e.get("bank_src","?")}')
    finally:
        s.close()
        p.terminate()
        try: p.wait(timeout=5)
        except subprocess.TimeoutExpired: p.kill()
        subprocess.run(['taskkill','/F','/IM','smw.exe'],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

if __name__ == '__main__':
    main()
