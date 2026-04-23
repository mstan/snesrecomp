"""Bug #8 — state-based first-divergence scan over player vars.

After both recomp and oracle reach GameMode=0x07 on their own timelines,
print a full byte-level diff of the player/physics zone ($0070-$009F)
for dwell 0 through MAX_DWELL. Shows EXACTLY which bytes diverge at
mode entry (attract-demo t=0) and how the divergence evolves over the
first few dwell frames.
"""
from __future__ import annotations
import json
import pathlib
import socket
import subprocess
import sys
import time


REPO = pathlib.Path(r'F:/Projects/SuperMarioWorldRecomp')
EXE = REPO / 'build/bin-x64-Oracle/smw.exe'
TARGET_MODE = 0x07
SCAN_LO = 0x0070
SCAN_HI = 0x009F
MAX_BOOT_FRAMES = 2000
MAX_DWELL = 5

# Labels for the scratch bytes we want to know about. Gaps left blank.
LABELS = {
    0x0071: 'PlayerAnimation',
    0x0072: 'PlayerInAir',
    0x0073: 'PlayerDucking',
    0x0074: 'PlayerFlight',
    0x0076: 'PlayerFacing',
    0x0077: 'PlayerBlockedDir',
    0x007a: 'PlayerXSpeed_lo',
    0x007b: 'PlayerXSpeed_hi',
    0x007c: 'PlayerYSpeed_lo',
    0x007d: 'PlayerYSpeed_hi',
    0x007e: 'PlayerXPosSub',
    0x007f: 'PlayerYPosSub',
    0x0080: 'PlayerXPosLo',
    0x0081: 'PlayerXPosHi',
    0x0082: 'PlayerYPosLo',
    0x0083: 'PlayerYPosHi',
    0x0084: 'YOnGroundRange',
    0x0086: 'PlayerMapPosX_lo',
    0x0087: 'PlayerMapPosX_hi',
    0x0088: 'PlayerMapPosY_lo',
    0x0089: 'PlayerMapPosY_hi',
    0x008a: 'PlayerBlockCol',
    0x008b: 'PlayerBlockColLo',
    0x008c: 'PlayerBlockColHi',
    0x008d: 'PlayerBlockSrc',
    0x008e: 'PlayerBlockX',
    0x008f: 'PlayerBlockY',
    0x0090: 'PlayerYPosInBlock',
    0x0091: 'PlayerBlockMoveY',
    0x0092: 'PlayerXPosInBlock',
    0x0093: 'PlayerBlockXSide',
    0x0094: 'PlayerXPosNext_lo',
    0x0095: 'PlayerXPosNext_hi',
    0x0096: 'PlayerYPosNext_lo',
    0x0097: 'PlayerYPosNext_hi',
    0x0098: 'TouchBlockYPos_lo',
    0x0099: 'TouchBlockYPos_hi',
    0x009a: 'TouchBlockXPos_lo',
    0x009b: 'TouchBlockXPos_hi',
    0x009e: 'PlayerSprite_ThisObj',
    0x009f: 'PlayerSprite_NextObj',
}


def cmd(sock, f, line):
    sock.sendall((line + '\n').encode())
    return json.loads(f.readline())


def step1(sock, f):
    before = cmd(sock, f, 'frame').get('frame', 0)
    cmd(sock, f, 'step 1')
    deadline = time.time() + 5
    while time.time() < deadline:
        if cmd(sock, f, 'frame').get('frame', 0) > before:
            return before + 1
        time.sleep(0.01)
    return before


def recomp_mode(sock, f):
    r = cmd(sock, f, 'dump_ram 0x100 1')
    return int(r['hex'].replace(' ', ''), 16)


def oracle_mode(sock, f):
    r = cmd(sock, f, 'emu_read_wram 0x100 1')
    return int(r['hex'].replace(' ', ''), 16) if r.get('ok') else None


def read_range_recomp(sock, f, lo, hi):
    n = hi - lo + 1
    r = cmd(sock, f, f'dump_ram 0x{lo:x} {n}')
    return bytes.fromhex(r['hex'].replace(' ', ''))


def read_range_oracle(sock, f, lo, hi):
    n = hi - lo + 1
    r = cmd(sock, f, f'emu_read_wram 0x{lo:x} {n}')
    return bytes.fromhex(r['hex'].replace(' ', ''))


def print_table(sock, f, dwell_label):
    rec = read_range_recomp(sock, f, SCAN_LO, SCAN_HI)
    ora = read_range_oracle(sock, f, SCAN_LO, SCAN_HI)
    diffs = []
    for i, (r, o) in enumerate(zip(rec, ora)):
        if r != o:
            a = SCAN_LO + i
            diffs.append((a, r, o, LABELS.get(a, '')))
    print(f'\n=== {dwell_label} : {len(diffs)} diffs in [${SCAN_LO:03x},${SCAN_HI:03x}] ===')
    print(f'  addr   r     o    label')
    for a, r, o, lbl in diffs:
        print(f'  0x{a:04x} 0x{r:02x}  0x{o:02x} {lbl}')


def main():
    if not EXE.exists():
        print(f'ERROR: Oracle exe not found at {EXE}', file=sys.stderr)
        return 1

    subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.5)
    proc = subprocess.Popen(
        [str(EXE), '--paused'],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        cwd=str(REPO),
    )

    try:
        sock = socket.socket()
        for _ in range(50):
            try: sock.connect(('127.0.0.1', 4377)); break
            except (ConnectionRefusedError, OSError): time.sleep(0.2)
        f = sock.makefile('r')
        banner = f.readline()
        if 'connected' not in banner:
            print(f'unexpected banner: {banner!r}', file=sys.stderr)
            return 1

        # Phase A: advance both until recomp GameMode=0x07.
        rframe_at_sync = None
        for frame in range(1, MAX_BOOT_FRAMES + 1):
            step1(sock, f)
            if recomp_mode(sock, f) == TARGET_MODE:
                rframe_at_sync = frame
                break
        if rframe_at_sync is None:
            print(f'FAIL: recomp GameMode=0x07 not reached')
            return 2

        # Phase B: advance oracle only until oracle GameMode=0x07.
        omode_now = oracle_mode(sock, f)
        oracle_extra = 0
        while omode_now != TARGET_MODE and oracle_extra < MAX_BOOT_FRAMES:
            cmd(sock, f, 'emu_step 1')
            oracle_extra += 1
            omode_now = oracle_mode(sock, f)
        if omode_now != TARGET_MODE:
            print(f'FAIL: oracle GameMode=0x07 not reached')
            return 2

        print(f'sync: recomp@frame{rframe_at_sync}, oracle+{oracle_extra} emu-only')

        print_table(sock, f, 'dwell=0 (both just entered mode 0x07)')
        for d in range(1, MAX_DWELL + 1):
            step1(sock, f)
            print_table(sock, f, f'dwell={d}')
        return 0
    finally:
        try: sock.close()
        except Exception: pass
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill()
        subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


if __name__ == '__main__':
    sys.exit(main())
