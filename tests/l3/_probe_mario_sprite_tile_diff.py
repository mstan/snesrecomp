"""Find first diverging OAM byte in Mario's sprite tiles (slots 64-79)
during the attract-demo jump that currently kills Mario.

OAM AoS layout:
  $0200+N*4 = X, +1 = Y, +2 = tile, +3 = attr
Slots 64-79 (Mario-area) tiles at $0302+[N-64]*4 etc.

Strategy:
  1. advance recomp + oracle to GameMode 0x07
  2. step in lock-step for N frames
  3. at each step, read full OAM ($0200-$03FF) from both sides
  4. report first frame where OAM bytes diverge, and which byte(s)

Minimal / read-only probe — no traces, no watches. Platform-agnostic.
"""
from __future__ import annotations
import json, pathlib, socket, subprocess, sys, time

REPO = pathlib.Path(r'F:/Projects/SuperMarioWorldRecomp')
EXE  = REPO / 'build/bin-x64-Oracle/smw.exe'

def cmd(s, f, line):
    s.sendall((line + '\n').encode()); return json.loads(f.readline())

def hex_to_bytes(h):
    return bytes.fromhex(h.replace(' ', ''))

def read_oam_recomp(s, f):
    # dump_ram <start_hex> <len_decimal>
    return hex_to_bytes(cmd(s, f, 'dump_ram 0x200 512')['hex'])

def read_oam_oracle(s, f):
    return hex_to_bytes(cmd(s, f, 'emu_read_wram 0x200 512')['hex'])

def dump_slot(oam, n):
    o = n * 4
    return f"slot{n:3d}: X={oam[o]:02x} Y={oam[o+1]:02x} tile={oam[o+2]:02x} attr={oam[o+3]:02x}"

def main():
    subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.5)
    p = subprocess.Popen([str(EXE), '--paused'], cwd=REPO,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        s = socket.socket()
        for _ in range(50):
            try: s.connect(('127.0.0.1', 4377)); break
            except (ConnectionRefusedError, OSError): time.sleep(0.2)
        f = s.makefile('r'); f.readline()

        # advance both sides to GameMode 0x07
        for _ in range(2000):
            cmd(s, f, 'step 1')
            gm = int(cmd(s, f, 'dump_ram 0x100 1')['hex'].replace(' ', ''), 16)
            if gm == 7: break
        for _ in range(2000):
            cmd(s, f, 'emu_step 1')
            gm = int(cmd(s, f, 'emu_read_wram 0x100 1')['hex'].replace(' ', ''), 16)
            if gm == 7: break

        print('=== both sides at GameMode 0x07 ===\n')

        # step in lock-step; report first OAM divergence
        first_div_frame = None
        first_div_offsets = []
        for frame in range(300):
            cmd(s, f, 'step 1')
            cmd(s, f, 'emu_step 1')
            r = read_oam_recomp(s, f)
            o = read_oam_oracle(s, f)
            diffs = [i for i in range(0x200) if r[i] != o[i]]
            # ignore slot 127 (often reserved / always-different due to boot)
            mario_diffs = [i for i in diffs if 0 <= i < 0x1C0]  # first 112 slots
            if mario_diffs and first_div_frame is None:
                first_div_frame = frame
                first_div_offsets = mario_diffs
                print(f'FIRST OAM DIVERGENCE at step {frame}')
                print(f'  total diverging bytes in slots 0-111: {len(mario_diffs)}')
                print(f'  offsets: {[hex(0x200+x) for x in mario_diffs[:40]]}\n')

                # show full diff per slot
                slots = sorted(set(x // 4 for x in mario_diffs))
                for sl in slots[:20]:
                    o_str = dump_slot(o, sl)
                    r_str = dump_slot(r, sl)
                    print(f'  oracle {o_str}')
                    print(f'  recomp {r_str}')
                    print()

                # capture context state
                print('--- context state (PlayerPose, Powerup, PlayerAnim, PlayerDirection, IFrameTimer) ---')
                for (label, cmd_r, cmd_o) in [
                    ('Powerup ($19)',        'dump_ram 0x19 1',     'emu_read_wram 0x19 1'),
                    ('PlayerDirection($76)', 'dump_ram 0x76 1',     'emu_read_wram 0x76 1'),
                    ('PlayerPose ($13E0)',   'dump_ram 0x13e0 2',   'emu_read_wram 0x13e0 2'),
                    ('PlayerAnim ($71)',     'dump_ram 0x71 1',     'emu_read_wram 0x71 1'),
                    ('IFrameTimer ($1497)',  'dump_ram 0x1497 1',   'emu_read_wram 0x1497 1'),
                    ('PlayerInAir ($72)',    'dump_ram 0x72 1',     'emu_read_wram 0x72 1'),
                    ('_4 ($04)',             'dump_ram 0x04 1',     'emu_read_wram 0x04 1'),
                    ('_5 ($05)',             'dump_ram 0x05 1',     'emu_read_wram 0x05 1'),
                    ('_6 ($06)',             'dump_ram 0x06 1',     'emu_read_wram 0x06 1'),
                ]:
                    rv = cmd(s, f, cmd_r)['hex']
                    ov = cmd(s, f, cmd_o)['hex']
                    match = '==' if rv == ov else '!='
                    print(f'  {label:28s}  recomp={rv}  {match}  oracle={ov}')
                break

        if first_div_frame is None:
            print('No OAM divergence in 300 frames (not expected).')
    finally:
        s.close()
        p.terminate()
        try: p.wait(timeout=5)
        except subprocess.TimeoutExpired: p.kill()
        subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

if __name__ == '__main__':
    main()
