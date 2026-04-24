"""Diff Mario's OAM slots during a jump on both sides.

Mario uses the first few OAM slots (0-3) for his body tiles. In SMW
these live at $0200 onward in WRAM (prep table, DMA'd to OAM each
NMI). Step both sides until Mario is in the air (PlayerInAir != 0),
then diff the OAM prep region to find which bytes of Mario's sprite
are malformed."""
from __future__ import annotations
import json, pathlib, socket, subprocess, sys, time
REPO = pathlib.Path(r'F:/Projects/SuperMarioWorldRecomp')
EXE = REPO / 'build/bin-x64-Oracle/smw.exe'


def cmd(s, f, line):
    s.sendall((line + '\n').encode()); return json.loads(f.readline())


def read_block(s, f, addr, n, emu=False):
    r = cmd(s, f, f'{"emu_read_wram" if emu else "dump_ram"} 0x{addr:x} {n}')
    h = r['hex'].replace(' ', '')
    return [int(h[2*i:2*i+2], 16) for i in range(n)]


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

    # Mode 0x07 on both
    for _ in range(2000):
        cmd(s, f, 'step 1')
        if int(cmd(s, f, 'dump_ram 0x100 1')['hex'].replace(' ', ''), 16) == 7: break
    for _ in range(2000):
        cmd(s, f, 'emu_step 1')
        if int(cmd(s, f, 'emu_read_wram 0x100 1')['hex'].replace(' ', ''), 16) == 7: break

    # Step lockstep until oracle Mario is in the air
    r_air_frame = None; o_air_frame = None
    for dwell in range(1, 200):
        cmd(s, f, 'step 1'); cmd(s, f, 'emu_step 1')
        r_air = int(cmd(s, f, 'dump_ram 0x72 1')['hex'].replace(' ', ''), 16)
        o_air = int(cmd(s, f, 'emu_read_wram 0x72 1')['hex'].replace(' ', ''), 16)
        if r_air and r_air_frame is None:
            r_air_frame = dwell
            print(f'recomp Mario enters air at dwell {dwell} (PlayerInAir=0x{r_air:02x})')
        if o_air and o_air_frame is None:
            o_air_frame = dwell
            print(f'oracle Mario enters air at dwell {dwell} (PlayerInAir=0x{o_air:02x})')
        if r_air_frame and o_air_frame: break
    if not (r_air_frame and o_air_frame):
        print('Mario never left ground within 200 frames')
        sys.exit(0)

    # Dump Mario's OAM prep region.
    # SMW per-sprite OAM prep table $0200-$03FF. Mario uses the first
    # few slots. Also dump OAMTileSize at $0420-$042B.
    OAM_BASE = 0x0200
    OAM_LEN = 512  # entire low-OAM prep region (128 slots)
    TILESIZE_BASE = 0x0420
    TILESIZE_LEN = 128  # one bit per OAM slot (packed differently, but dump more)
    r_oam = read_block(s, f, OAM_BASE, OAM_LEN)
    o_oam = read_block(s, f, OAM_BASE, OAM_LEN, emu=True)
    r_ts = read_block(s, f, TILESIZE_BASE, TILESIZE_LEN)
    o_ts = read_block(s, f, TILESIZE_BASE, TILESIZE_LEN, emu=True)

    # Only print slots that differ OR aren't the F0 blank pattern.
    print(f'\n=== OAM diffs ({OAM_LEN // 4} slots) ===')
    print(f'{"slot":4} {"field":8} {"oracle":>6}  {"recomp":>6}  diff')
    n_slots = OAM_LEN // 4
    for slot in range(n_slots):
        o_blank = all(o_oam[slot*4+i] in (0x00, 0xf0) for i in range(4))
        r_blank = all(r_oam[slot*4+i] in (0x00, 0xf0) for i in range(4))
        any_diff = any(o_oam[slot*4+i] != r_oam[slot*4+i] for i in range(4))
        if o_blank and r_blank and not any_diff:
            continue
        for i, fld in enumerate(('Y', 'TileNo', 'Attr', 'X')):
            oi = slot * 4 + i
            o = o_oam[oi]; r = r_oam[oi]
            mark = '  !=' if o != r else ''
            print(f'  {slot:3d}  {fld:8s}   0x{o:02x}    0x{r:02x}  {mark}')
    # OAMTileSize differences only
    diffs = [(i, o_ts[i], r_ts[i]) for i in range(TILESIZE_LEN) if o_ts[i] != r_ts[i]]
    print(f'\n=== OAMTileSize $0420-$0{TILESIZE_BASE+TILESIZE_LEN-1:03x}: {len(diffs)} differing entries ===')
    for i, o, r in diffs[:20]:
        print(f'  offset {i:3d}  oracle=0x{o:02x}  recomp=0x{r:02x}')

    # Also show Mario's position & animation so we can cross-check
    for reg, name in [(0x71, 'Animation'), (0x72, 'InAir'),
                       (0x73, 'IsDucked'), (0x13, 'TrueFrame'),
                       (0x96, 'YPosNext'), (0x94, 'XPosNext'),
                       (0x19, 'PlayerPowerup'),
                       (0x73, 'Ducking'),
                       (0x1470, 'CapeSpin'),
                       (0x74, 'ClimbingStatus'),
                       (0x75, 'InWater'),
                       (0x76, 'FacingDirection'),
                       (0x77, 'SideBlocks'),
                       (0x13e2, 'PlayerFacingDirection'),
                       (0x05, '_5'), (0x06, '_6'), (0x04, '_4'),
                       (0x16, 'Controller_1'),
                       (0x17, 'Controller_2'),
                       (0x13ef, 'PlayerIsOnGround'),
                       (0x149F, 'PlayerHiddenTiles'),
                       (0x13de, 'MarioFrame'),
                       (0x1497, 'MarioSwimPoseIdx'),
                       (0x13e3, 'PlayerImageBlock'),
                       (0x73c, 'PlayerDirections'),
                       ]:
        w = 2 if reg in (0x96, 0x94) else 1
        ro = read_block(s, f, reg, w)
        oo = read_block(s, f, reg, w, emu=True)
        rv = sum(b << (8*i) for i, b in enumerate(ro))
        ov = sum(b << (8*i) for i, b in enumerate(oo))
        print(f'  {name:14s}  oracle=0x{ov:0{w*2}x}  recomp=0x{rv:0{w*2}x}')
finally:
    s.close()
    p.terminate()
    try: p.wait(timeout=5)
    except subprocess.TimeoutExpired: p.kill()
    subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
