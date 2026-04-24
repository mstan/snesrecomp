"""Wide direct-page + key WRAM diff between recomp and oracle over
first 10 frames after GameMode=0x07, to find the earliest diverging byte."""
from __future__ import annotations
import json, pathlib, socket, subprocess, time

REPO = pathlib.Path(r'F:/Projects/SuperMarioWorldRecomp')
EXE  = REPO / 'build/bin-x64-Oracle/smw.exe'

def cmd(s, f, line):
    s.sendall((line + '\n').encode()); return json.loads(f.readline())

def hexbytes(resp):
    return bytes.fromhex(resp['hex'].replace(' ', ''))

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

        # both sides to GameMode 0x07
        for _ in range(2000):
            cmd(s, f, 'step 1')
            if int(cmd(s, f, 'dump_ram 0x100 1')['hex'].replace(' ',''),16) == 7: break
        for _ in range(2000):
            cmd(s, f, 'emu_step 1')
            if int(cmd(s, f, 'emu_read_wram 0x100 1')['hex'].replace(' ',''),16) == 7: break

        print('=== both at GM=0x07; diffing DP ($00-$FF), $100-$1FF, $1E00-$1FFF, $13E0-$13FF per step ===\n')
        regions = [
            ('DP',      '0x00',   256),
            ('$0100',   '0x100',  256),   # game state block
            ('$1E00',   '0x1e00', 256),   # sprite state block
            ('$13E0',   '0x13e0', 32),    # player pose/timers
            ('$0300',   '0x300',  256),   # OAM tile bytes (back half)
        ]
        for step in range(6):
            cmd(s, f, 'step 1'); cmd(s, f, 'emu_step 1')
            print(f'--- step {step} ---')
            for (label, addr, ln) in regions:
                r = hexbytes(cmd(s, f, f'dump_ram {addr} {ln}'))
                o = hexbytes(cmd(s, f, f'emu_read_wram {addr} {ln}'))
                diffs = [i for i in range(min(len(r), len(o))) if r[i] != o[i]]
                if not diffs:
                    print(f'  {label:10s} ok ({ln} bytes match)')
                    continue
                base = int(addr, 16)
                print(f'  {label:10s} {len(diffs)} diffs:')
                for i in diffs[:25]:
                    print(f'    {hex(base+i):>8s}  recomp={r[i]:02x}  oracle={o[i]:02x}')
                if len(diffs) > 25:
                    print(f'    ... +{len(diffs)-25} more')
            print()
    finally:
        s.close(); p.terminate()
        try: p.wait(timeout=5)
        except subprocess.TimeoutExpired: p.kill()
        subprocess.run(['taskkill','/F','/IM','smw.exe'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

if __name__ == '__main__':
    main()
