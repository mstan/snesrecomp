"""Compare full OAM between recomp and oracle at dwell=60 (when Mario
is mid-jump in the attract demo). Identify Mario's slots by looking
for tile values in the small-Mario/big-Mario range with visible Y."""
from __future__ import annotations
import json, pathlib, socket, subprocess, time

REPO = pathlib.Path(r'F:/Projects/SuperMarioWorldRecomp')
EXE  = REPO / 'build/bin-x64-Oracle/smw.exe'

def cmd(s, f, line):
    s.sendall((line + '\n').encode()); return json.loads(f.readline())

def hexbytes(resp):
    return bytes.fromhex(resp['hex'].replace(' ', ''))

def fmt_slot(oam, n):
    o = n * 4
    return f'X={oam[o]:02x} Y={oam[o+1]:02x} tile={oam[o+2]:02x} attr={oam[o+3]:02x}'

def main():
    subprocess.run(['taskkill','/F','/IM','smw.exe'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.5)
    p = subprocess.Popen([str(EXE),'--paused'], cwd=REPO, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        s = socket.socket()
        for _ in range(50):
            try: s.connect(('127.0.0.1',4377)); break
            except (ConnectionRefusedError, OSError): time.sleep(0.2)
        f = s.makefile('r'); f.readline()

        # both sides to GM=0x07
        for _ in range(2000):
            cmd(s, f, 'step 1')
            if int(cmd(s, f, 'dump_ram 0x100 1')['hex'].replace(' ',''),16) == 7: break
        for _ in range(2000):
            cmd(s, f, 'emu_step 1')
            if int(cmd(s, f, 'emu_read_wram 0x100 1')['hex'].replace(' ',''),16) == 7: break

        # dwell 60 frames (Mario mid-demo)
        for _ in range(60):
            cmd(s, f, 'step 1'); cmd(s, f, 'emu_step 1')

        # read full OAM both sides
        r = hexbytes(cmd(s, f, 'dump_ram 0x200 512'))
        o = hexbytes(cmd(s, f, 'emu_read_wram 0x200 512'))

        print(f'=== full OAM diff at dwell 60 post-GM=0x07 ===')
        print(f'recomp OAM len={len(r)}, oracle OAM len={len(o)}')

        diffs = [i for i in range(min(len(r),len(o))) if r[i] != o[i]]
        print(f'\ntotal byte diffs: {len(diffs)}')

        # group by slot
        slot_diffs = {}
        for i in diffs:
            sl = i // 4
            slot_diffs.setdefault(sl, []).append(i)

        # print every slot that differs
        print(f'\ndiverging slots: {len(slot_diffs)}')
        for sl in sorted(slot_diffs.keys())[:40]:
            o_y = o[sl*4+1]; r_y = r[sl*4+1]
            o_visible = o_y not in (0xF0, 0xE0, 0xD0)
            r_visible = r_y not in (0xF0, 0xE0, 0xD0)
            mark = ''
            if o_visible and not r_visible: mark = ' <-- oracle ON-SCREEN, recomp HIDDEN'
            elif r_visible and not o_visible: mark = ' <-- recomp ON-SCREEN, oracle HIDDEN'
            elif o_visible and r_visible: mark = ' <-- both on-screen, differ'
            print(f'slot{sl:3d}: oracle({fmt_slot(o,sl)})  recomp({fmt_slot(r,sl)}){mark}')

        # capture key state
        print('\n--- context state at dwell 60 ---')
        for (label, addr, ln) in [
            ('Powerup',           0x19,   1),
            ('PlayerAnim',        0x71,   1),
            ('PlayerInAir',       0x72,   1),
            ('PlayerDirection',   0x76,   1),
            ('PlayerPose',        0x13e0, 2),
            ('IFrameTimer',       0x1497, 1),
            ('PlayerXPosNext',    0x94,   2),
            ('PlayerYPosNext',    0x96,   2),
            ('Layer1XPos',        0x1a,   2),
            ('Layer1YPos',        0x1c,   2),
            ('PlayerXPosScrRel',  0x7e,   2),
            ('PlayerYPosScrRel',  0x80,   2),
        ]:
            rv = cmd(s, f, f'dump_ram 0x{addr:x} {ln}')['hex']
            ov = cmd(s, f, f'emu_read_wram 0x{addr:x} {ln}')['hex']
            mm = '==' if rv == ov else '!='
            print(f'  {label:20s} recomp={rv}  {mm}  oracle={ov}')
    finally:
        s.close(); p.terminate()
        try: p.wait(timeout=5)
        except subprocess.TimeoutExpired: p.kill()
        subprocess.run(['taskkill','/F','/IM','smw.exe'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

if __name__ == '__main__':
    main()
