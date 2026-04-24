"""At frame 17 post-GM=07 (state-synced to Pose=0b, InAir=0b, Anim=00),
dump all state that MarioGFXDMA reads, to find which non-synced
input drives the VRAM divergence.

SMW MarioGFXDMA ($00:A300) reads:
  $19   Powerup
  $76   PlayerDirection
  $13E0 PlayerPose
  $71   PlayerAnimation (= anim frame)
  $0D84 PlayerGfxTileCount
  $DBB  FreezeSpriteFlag? / PlayerGfxPtr-adjacent
Plus the DP scratch vars it writes during upload ($00-$0F area).
"""
from __future__ import annotations
import json, pathlib, socket, subprocess, time

REPO = pathlib.Path(r'F:/Projects/SuperMarioWorldRecomp')
EXE  = REPO / 'build/bin-x64-Oracle/smw.exe'

def cmd(s, f, line):
    s.sendall((line + '\n').encode()); return json.loads(f.readline())

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

        # Step forward to frame where both sides are mid-jump Pose=0b InAir=0b
        # (per prior probe, frame 17 is first "both synced to jumping" with big VRAM div)
        for _ in range(17):
            cmd(s, f, 'step 1'); cmd(s, f, 'emu_step 1')

        print('=== State at frame 17 post-GM=07 (state-synced jumping) ===\n')

        # Comprehensive state dump for MarioGFXDMA inputs
        state_bytes = [
            ('DP $00-$1F',       0x00,   32),
            ('DP $70-$8F',       0x70,   32),
            ('Powerup $19',      0x19,    1),
            ('PlayerAnim $71',   0x71,    1),
            ('PlayerInAir $72',  0x72,    1),
            ('PlayerDir $76',    0x76,    1),
            ('IFrameTimer $1497',0x1497,  1),
            ('PlayerPose $13E0', 0x13E0,  2),
            ('PlayerPoseLen $13E5', 0x13E5, 1),
            ('CapePose $13E3',   0x13E3,  1),
            ('TurnLvl $14AD',    0x14AD,  1),
            ('MusicBackup $14AE',0x14AE,  1),
            ('Mosaic $1493',     0x1493,  1),
            ('MarioHeldItem $148F', 0x148F, 1),
            ('PlayerGfxTileCount $0D84', 0x0D84, 1),
            ('PlayerPalletePtr $0D82', 0x0D82, 2),
            ('DynGfxTilePtr $0D85[20]', 0x0D85, 20),
            ('DynGfxTile7FPtr $0D99', 0x0D99, 2),
            ('WallrunType $13FD',0x13FD,  1),
            ('PlayerXPosScrRel $7E', 0x7E, 2),
            ('PlayerYPosScrRel $80', 0x80, 2),
            ('PlayerXPos $94',   0x94,    2),
            ('PlayerYPos $96',   0x96,    2),
            ('Layer1X $1A',      0x1A,    2),
            ('Layer1Y $1C',      0x1C,    2),
        ]

        any_diff = False
        for (label, a, ln) in state_bytes:
            rv = cmd(s, f, f'dump_ram 0x{a:x} {ln}')['hex']
            ov = cmd(s, f, f'emu_read_wram 0x{a:x} {ln}')['hex']
            if rv == ov:
                print(f'  OK {label:32s} {rv}')
            else:
                print(f'  != {label:32s} recomp={rv}  oracle={ov}')
                any_diff = True

        print(f'\nAny diff: {any_diff}')
    finally:
        s.close(); p.terminate()
        try: p.wait(timeout=5)
        except subprocess.TimeoutExpired: p.kill()
        subprocess.run(['taskkill','/F','/IM','smw.exe'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

if __name__ == '__main__':
    main()
