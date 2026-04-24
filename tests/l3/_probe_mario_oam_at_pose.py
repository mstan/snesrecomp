"""State-synced comparison: advance each side independently until
Mario is at PlayerPose=0x24 (jumping), PlayerInAir>0, PlayerPowerup=0.
Then diff OAM (including hi-table) to find where the sprite differs.

This removes any frame-step-rate mismatch between the two emulators.
"""
from __future__ import annotations
import json, pathlib, socket, subprocess, time

REPO = pathlib.Path(r'F:/Projects/SuperMarioWorldRecomp')
EXE  = REPO / 'build/bin-x64-Oracle/smw.exe'

def cmd(s, f, line):
    s.sendall((line + '\n').encode()); return json.loads(f.readline())
def hb(r): return bytes.fromhex(r['hex'].replace(' ',''))

def wait_for(s, f, side, cond_func, max_frames=5000):
    step_cmd = 'step 1' if side == 'recomp' else 'emu_step 1'
    for _ in range(max_frames):
        cmd(s, f, step_cmd)
        if cond_func(): return True
    return False

def slot_size(hi, n):
    return (hi[n // 4] >> ((n % 4) * 2)) & 3

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

        def r_pose(): return int(cmd(s,f,'dump_ram 0x13e0 1')['hex'].replace(' ',''),16)
        def o_pose(): return int(cmd(s,f,'emu_read_wram 0x13e0 1')['hex'].replace(' ',''),16)
        def r_inair(): return int(cmd(s,f,'dump_ram 0x72 1')['hex'].replace(' ',''),16)
        def o_inair(): return int(cmd(s,f,'emu_read_wram 0x72 1')['hex'].replace(' ',''),16)
        def r_gm(): return int(cmd(s,f,'dump_ram 0x100 1')['hex'].replace(' ',''),16)
        def o_gm(): return int(cmd(s,f,'emu_read_wram 0x100 1')['hex'].replace(' ',''),16)

        # 1) advance each side to GameMode=0x07 (attract demo active)
        print('Advancing recomp to GM=0x07...')
        wait_for(s, f, 'recomp', lambda: r_gm() == 7)
        print('Advancing oracle to GM=0x07...')
        wait_for(s, f, 'oracle', lambda: o_gm() == 7)

        # 2) then each side to PlayerInAir != 0 AND GM still 0x07 (Mario jumping in demo)
        print('Advancing recomp to first PlayerInAir>0 in GM=0x07...')
        wait_for(s, f, 'recomp', lambda: r_inair() != 0 and r_gm() == 7)
        print(f'  recomp stopped: InAir={r_inair():02x} Pose={r_pose():02x} GM={r_gm():02x}')

        print('Advancing oracle to first PlayerInAir>0 in GM=0x07...')
        wait_for(s, f, 'oracle', lambda: o_inair() != 0 and o_gm() == 7)
        print(f'  oracle stopped: InAir={o_inair():02x} Pose={o_pose():02x} GM={o_gm():02x}')

        # 3) step both sides 2 more frames so OAM has been populated by rendering
        for _ in range(2):
            cmd(s, f, 'step 1'); cmd(s, f, 'emu_step 1')

        # full state dump & OAM
        print('\n=== state-synced (both at first jump frame) ===')
        for (label, a, ln) in [
            ('GameMode',       0x100, 1),
            ('Powerup',        0x19,  1),
            ('PlayerPose',     0x13e0, 1),
            ('PlayerAnim',     0x71,  1),
            ('PlayerInAir',    0x72,  1),
            ('PlayerDirection',0x76,  1),
            ('PlayerXPosNext', 0x94,  2),
            ('PlayerYPosNext', 0x96,  2),
            ('_4',             0x04,  1),
            ('_5',             0x05,  1),
            ('_6',             0x06,  1),
        ]:
            rv = cmd(s,f,f'dump_ram 0x{a:x} {ln}')['hex']
            ov = cmd(s,f,f'emu_read_wram 0x{a:x} {ln}')['hex']
            mm = '==' if rv == ov else '!='
            print(f'  {label:18s} recomp={rv}  {mm}  oracle={ov}')

        r_oam = hb(cmd(s,f,'dump_ram 0x200 512'))
        o_oam = hb(cmd(s,f,'emu_read_wram 0x200 512'))
        r_hi = hb(cmd(s,f,'dump_ram 0x420 32'))
        o_hi = hb(cmd(s,f,'emu_read_wram 0x420 32'))

        print(f'\nOAM byte diffs ($0200-$03FF): {sum(1 for i in range(512) if r_oam[i]!=o_oam[i])}/512')
        print(f'OAM hi-table diffs ($0420-$043F): {sum(1 for i in range(32) if r_hi[i]!=o_hi[i])}/32')

        # Find Mario's slot (small-tile visible sprite)
        print('\n=== Mario-candidate slots (visible small-tile) ===')
        for sl in range(128):
            rx, ry, rt, ra = r_oam[sl*4:sl*4+4]
            ox, oy, ot, oa = o_oam[sl*4:sl*4+4]
            # skip if both sides hidden
            r_hid = ry in (0xf0, 0xe0)
            o_hid = oy in (0xf0, 0xe0)
            if r_hid and o_hid: continue
            # skip if both tiles identical
            if rx==ox and ry==oy and rt==ot and ra==oa and slot_size(r_hi,sl)==slot_size(o_hi,sl): continue
            rs = slot_size(r_hi, sl); os_ = slot_size(o_hi, sl)
            print(f'  sl{sl:3d}  oracle(X={ox:02x} Y={oy:02x} tile={ot:02x} attr={oa:02x} sz={os_})  '
                  f'recomp(X={rx:02x} Y={ry:02x} tile={rt:02x} attr={ra:02x} sz={rs})')
    finally:
        s.close(); p.terminate()
        try: p.wait(timeout=5)
        except subprocess.TimeoutExpired: p.kill()
        subprocess.run(['taskkill','/F','/IM','smw.exe'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

if __name__ == '__main__':
    main()
