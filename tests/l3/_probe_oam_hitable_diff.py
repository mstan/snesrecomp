"""Diff OAM hi-table ($0420-$043F) between recomp and oracle at dwell 60.
Hi-table holds 2 bits per slot: bit0=X9 (X>=256), bit1=size (0=8x8, 1=16x16).
Each byte encodes 4 slots: slots N*4+0..N*4+3 in byte N.

Also cross-check slot 68/69 size interpretation to test the
"small-mario rendered as 16x16" hypothesis for user's visual bug."""
from __future__ import annotations
import json, pathlib, socket, subprocess, time

REPO = pathlib.Path(r'F:/Projects/SuperMarioWorldRecomp')
EXE  = REPO / 'build/bin-x64-Oracle/smw.exe'

def cmd(s, f, line):
    s.sendall((line + '\n').encode()); return json.loads(f.readline())
def hexbytes(r): return bytes.fromhex(r['hex'].replace(' ', ''))

def slot_bits(hi, n):
    byte = hi[n // 4]
    shift = (n % 4) * 2
    bits = (byte >> shift) & 3
    return bits

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

        for _ in range(2000):
            cmd(s, f, 'step 1')
            if int(cmd(s, f, 'dump_ram 0x100 1')['hex'].replace(' ',''),16) == 7: break
        for _ in range(2000):
            cmd(s, f, 'emu_step 1')
            if int(cmd(s, f, 'emu_read_wram 0x100 1')['hex'].replace(' ',''),16) == 7: break

        # dwell 60 (Mario mid-jump per earlier probe)
        print('=== OAM hi-table diff + Mario slots at dwell 60 ===\n')
        for _ in range(60):
            cmd(s, f, 'step 1'); cmd(s, f, 'emu_step 1')
        for dwell in (60,):
            pass
            r_hi = hexbytes(cmd(s, f, 'dump_ram 0x420 32'))
            o_hi = hexbytes(cmd(s, f, 'emu_read_wram 0x420 32'))
            r_oam = hexbytes(cmd(s, f, 'dump_ram 0x200 512'))
            o_oam = hexbytes(cmd(s, f, 'emu_read_wram 0x200 512'))
            pose_r = cmd(s, f, 'dump_ram 0x13e0 2')['hex']
            pose_o = cmd(s, f, 'emu_read_wram 0x13e0 2')['hex']
            inair_r = cmd(s, f, 'dump_ram 0x72 1')['hex']
            inair_o = cmd(s, f, 'emu_read_wram 0x72 1')['hex']

            # find Mario's slots by looking for consecutive small-tile writes in visible Y
            def find_mario(oam):
                for sl in range(0, 127):
                    o = sl * 4
                    x, y, tile, attr = oam[o], oam[o+1], oam[o+2], oam[o+3]
                    if y in (0xF0, 0xE0, 0xD0): continue
                    if tile <= 0x20:  # small mario range
                        return sl
                return None

            hi_diffs = [i for i in range(32) if r_hi[i] != o_hi[i]]

            print(f'--- dwell {dwell} ---')
            print(f'  PlayerPose recomp={pose_r} oracle={pose_o}  PlayerInAir recomp={inair_r} oracle={inair_o}')
            print(f'  OAM hi-table diffs: {len(hi_diffs)}  {[hex(0x420+i) for i in hi_diffs[:10]]}')
            # show size bits for slots 60-80 both sides
            print(f'  slot size bits (size=1 means 16x16):')
            for sl in range(60, 80):
                rb = slot_bits(r_hi, sl); ob = slot_bits(o_hi, sl)
                rtile = r_oam[sl*4+2]; otile = o_oam[sl*4+2]
                ry = r_oam[sl*4+1]; oy = o_oam[sl*4+1]
                r_vis = 'vis' if ry not in (0xF0, 0xE0) else 'hid'
                o_vis = 'vis' if oy not in (0xF0, 0xE0) else 'hid'
                mark = '' if (rb == ob and rtile == otile) else '  <--'
                print(f'    sl{sl:3d} recomp(size={rb} tile={rtile:02x} {r_vis})  oracle(size={ob} tile={otile:02x} {o_vis}){mark}')
            print()
    finally:
        s.close(); p.terminate()
        try: p.wait(timeout=5)
        except subprocess.TimeoutExpired: p.kill()
        subprocess.run(['taskkill','/F','/IM','smw.exe'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

if __name__ == '__main__':
    main()
