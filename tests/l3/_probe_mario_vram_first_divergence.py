"""Find the first frame in GM=0x07 where Mario's OBJ VRAM tiles diverge.

$C000 byte = $6000 word = OBJ VRAM base in SMW. Mario's small-sprite
tile bitmaps (tiles 0x00-0x0F) occupy $C000-$C1FF = 512 bytes.

Strategy:
  1. Advance each side to GM=0x07 independently.
  2. Step both sides 1 frame at a time in lock-step.
  3. At each step, read $C000-$C1FF both sides. If they diverge,
     capture full context (Mario state + first diverging byte).
  4. Bail at ~80 frames (before Mario dies and recomp crashes).
"""
from __future__ import annotations
import json, pathlib, socket, subprocess, time

REPO = pathlib.Path(r'F:/Projects/SuperMarioWorldRecomp')
EXE  = REPO / 'build/bin-x64-Oracle/smw.exe'

def cmd(s, f, line):
    s.sendall((line + '\n').encode()); return json.loads(f.readline())
def hb(r): return bytes.fromhex(r['hex'].replace(' ',''))

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

        for _ in range(3000):
            cmd(s, f, 'step 1')
            if int(cmd(s, f, 'dump_ram 0x100 1')['hex'].replace(' ',''),16) == 7: break
        for _ in range(3000):
            cmd(s, f, 'emu_step 1')
            if int(cmd(s, f, 'emu_read_wram 0x100 1')['hex'].replace(' ',''),16) == 7: break

        print('=== per-frame OBJ VRAM $C000-$C1FF comparison ===')
        print('frame  pose  inair  anim  pos(X,Y)      first_vram_diff')

        for frame in range(80):
            cmd(s, f, 'step 1')
            cmd(s, f, 'emu_step 1')

            r_v = hb(cmd(s, f, 'dump_vram 0xc000 512'))
            o_v = hb(cmd(s, f, 'emu_read_vram 0xc000 512'))

            # state capture
            pose   = int(cmd(s,f,'dump_ram 0x13e0 1')['hex'].replace(' ',''),16)
            opose  = int(cmd(s,f,'emu_read_wram 0x13e0 1')['hex'].replace(' ',''),16)
            inair  = int(cmd(s,f,'dump_ram 0x72 1')['hex'].replace(' ',''),16)
            oinair = int(cmd(s,f,'emu_read_wram 0x72 1')['hex'].replace(' ',''),16)
            anim   = int(cmd(s,f,'dump_ram 0x71 1')['hex'].replace(' ',''),16)
            oanim  = int(cmd(s,f,'emu_read_wram 0x71 1')['hex'].replace(' ',''),16)
            xp_r   = cmd(s,f,'dump_ram 0x94 2')['hex']
            yp_r   = cmd(s,f,'dump_ram 0x96 2')['hex']
            xp_o   = cmd(s,f,'emu_read_wram 0x94 2')['hex']
            yp_o   = cmd(s,f,'emu_read_wram 0x96 2')['hex']

            diffs = [i for i in range(len(r_v)) if r_v[i] != o_v[i]]
            state_synced = (pose==opose and inair==oinair and anim==oanim and xp_r==xp_o and yp_r==yp_o)
            marker = '[SYNCED]' if state_synced else '[DRIFT ]'
            if diffs:
                first = diffs[0]
                print(f'{frame:3d}    {pose:02x}/{opose:02x} {inair:02x}/{oinair:02x} {anim:02x}/{oanim:02x} '
                      f'({xp_r}/{xp_o},{yp_r}/{yp_o}) {marker} '
                      f'first@0x{0xc000+first:04x} r={r_v[first]:02x} o={o_v[first]:02x} ({len(diffs)} diffs total)')
            else:
                print(f'{frame:3d}    {pose:02x}/{opose:02x} {inair:02x}/{oinair:02x} {anim:02x}/{oanim:02x} '
                      f'({xp_r}/{xp_o},{yp_r}/{yp_o}) {marker} VRAM match')
    finally:
        s.close(); p.terminate()
        try: p.wait(timeout=5)
        except subprocess.TimeoutExpired: p.kill()
        subprocess.run(['taskkill','/F','/IM','smw.exe'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

if __name__ == '__main__':
    main()
