"""Frozen koopa investigation — step 1: state-sync sprite RAM diff.

Per docs/GOLDEN_TESTING.md: state-sync recomp + oracle on mode 0x07
entry, then diff the sprite tables at increasing dwell. The seed byte
is the FIRST sprite-state byte that differs.

SMW sprite table layout (12 slots, indexed X = 0..11):
  $14C8+X  SpriteStatus       (00=empty, 08=normal, ...)
  $14E0+X  SpriteNumber       (the koopa is sprite #04)
  $14D4+X  SpriteYPosHigh     paired with $00D8+X for YPos lo
  $14E8+X  SpriteXPosHigh     paired with $00E4+X for XPos lo
  $00AA+X  SpriteYSpeed       (signed)
  $00B6+X  SpriteXSpeed       (signed)
  $00C2+X  SpriteState
  $1540+X  SpriteOffscreen
  $151C+X  SpriteAniFrame
  $1534+X  SpriteIsActive
  $157C+X  SpriteDir
  $1594+X  SpriteIsBlocked
  $15A0+X  SpriteIsKilled

If the koopa is "frozen", the most likely seed bytes are:
  - $14C8+X SpriteStatus stuck in non-active value
  - $00B6+X SpriteXSpeed = 0 on recomp, non-zero on oracle
  - $1534+X SpriteIsActive = 0 on recomp
  - or some upstream "process this slot this frame" gate

This probe scans full sprite tables at multiple dwell points and
prints labeled byte-level diffs."""
from __future__ import annotations
import json, pathlib, socket, subprocess, sys, time

REPO = pathlib.Path(r'F:/Projects/SuperMarioWorldRecomp')
EXE = REPO / 'build/bin-x64-Oracle/smw.exe'
TARGET_MODE = 0x07
DWELLS = [30, 60, 120, 180, 300]
MAX_BOOT = 2000

# Sprite table bases per slot (offset = slot index 0..11)
SPRITE_TABLES = [
    (0x14C8, 'SpriteStatus'),
    (0x14E0, 'SpriteNumber'),
    (0x00D8, 'SpriteYPosLo'),
    (0x14D4, 'SpriteYPosHi'),
    (0x00E4, 'SpriteXPosLo'),
    (0x14E8, 'SpriteXPosHi'),
    (0x00AA, 'SpriteYSpeed'),
    (0x00B6, 'SpriteXSpeed'),
    (0x00C2, 'SpriteState'),
    (0x1540, 'SpriteOffscreen'),
    (0x151C, 'SpriteAniFrame'),
    (0x1528, 'SpriteOAMTileset'),
    (0x1534, 'SpriteIsActive'),
    (0x1558, 'SpriteImmunity'),
    (0x157C, 'SpriteDir'),
    (0x1594, 'SpriteIsBlocked'),
    (0x15A0, 'SpriteIsKilled'),
    (0x15AC, 'SpriteSubObjectKills'),
    (0x15B8, 'SpriteIsBeingEaten'),
    (0x15C4, 'SpriteIsBlockSpawned'),
]
NUM_SLOTS = 12


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


def r_byte(s, f, addr):
    return int(cmd(s, f, f'dump_ram 0x{addr:x} 1')['hex'].replace(' ', ''), 16)


def e_byte(s, f, addr):
    r = cmd(s, f, f'emu_read_wram 0x{addr:x} 1')
    return int(r['hex'].replace(' ', ''), 16) if r.get('ok') else -1


def snapshot_sprites(s, f, reader):
    """Return {(slot, label): value} for all sprite slots."""
    out = {}
    for base, label in SPRITE_TABLES:
        for slot in range(NUM_SLOTS):
            out[(slot, label)] = reader(s, f, base + slot)
    return out


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

        # Phase A: advance both until recomp GameMode=0x07.
        rframe = None
        for fr in range(1, MAX_BOOT + 1):
            step1(s, f)
            if r_byte(s, f, 0x100) == TARGET_MODE:
                rframe = fr; break
        if rframe is None: print('FAIL: recomp 0x07'); return 2

        # Phase B: oracle alone until oracle GameMode=0x07.
        extra = 0
        while e_byte(s, f, 0x100) != TARGET_MODE and extra < MAX_BOOT:
            cmd(s, f, 'emu_step 1'); extra += 1
        if e_byte(s, f, 0x100) != TARGET_MODE: print('FAIL: oracle 0x07'); return 2

        print(f'sync: recomp@f{rframe}, oracle+{extra} emu-only frames')

        # Walk through dwell points.
        prev_dwell = 0
        for dwell in DWELLS:
            for _ in range(dwell - prev_dwell):
                step1(s, f)
            prev_dwell = dwell

            rec = snapshot_sprites(s, f, r_byte)
            ora = snapshot_sprites(s, f, e_byte)

            diffs = [(slot, label, rec[(slot, label)], ora[(slot, label)])
                     for (slot, label) in rec
                     if rec[(slot, label)] != ora[(slot, label)]]
            print(f'\n=== dwell={dwell} : {len(diffs)} sprite-byte diffs ===')

            # Group by slot first.
            slots_with_diffs = sorted(set(s for s, _, _, _ in diffs))
            for slot in slots_with_diffs:
                slot_diffs = [(l, rv, ov) for (s_, l, rv, ov) in diffs if s_ == slot]
                # Show slot summary: status & number first
                rec_status = rec[(slot, 'SpriteStatus')]
                rec_number = rec[(slot, 'SpriteNumber')]
                ora_status = ora[(slot, 'SpriteStatus')]
                ora_number = ora[(slot, 'SpriteNumber')]
                print(f'  slot {slot:2}: '
                      f'recomp(status=0x{rec_status:02x},num=0x{rec_number:02x}) '
                      f'oracle(status=0x{ora_status:02x},num=0x{ora_number:02x})')
                for label, rv, ov in slot_diffs:
                    print(f'      {label:24} recomp=0x{rv:02x} oracle=0x{ov:02x}')
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
