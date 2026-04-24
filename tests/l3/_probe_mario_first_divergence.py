"""Bisect first frame where recomp's Mario state diverges from oracle
during the attract demo. Tracks a minimal Mario state: position,
speeds, animation, on-ground flag, touch-block positions.

Uses Oracle build's embedded snes9x as the oracle — advance both
simultaneously via step / emu_step, read at each frame, diff.
"""
from __future__ import annotations
import json, pathlib, socket, subprocess, sys, time

REPO = pathlib.Path(r'F:/Projects/SuperMarioWorldRecomp')
EXE = REPO / 'build/bin-x64-Oracle/smw.exe'

# Minimal Mario state: address -> (label, width)
STATE = {
    0x0071: ('PlayerAnimation', 1),
    0x0072: ('PlayerInAir', 1),
    0x007b: ('PlayerXSpeed', 1),
    0x007d: ('PlayerYSpeed', 1),
    0x0090: ('PlayerYPosInBlock', 1),
    0x0091: ('PlayerBlockMoveY', 1),
    0x0092: ('PlayerXPosInBlock', 1),
    0x0093: ('PlayerBlockXSide', 1),
    0x0094: ('PlayerXPosNext', 2),
    0x0096: ('PlayerYPosNext', 2),
    0x0098: ('TouchBlockYPos', 2),
    0x009a: ('TouchBlockXPos', 2),
    0x13ef: ('PlayerIsOnGround', 1),
    0x1471: ('PlayerStandingOnTileType', 1),
    0x13e0: ('PlayerDrawY', 2),
}


def cmd(sock, f, line):
    sock.sendall((line + '\n').encode())
    return json.loads(f.readline())


def read_state(sock, f, emu=False):
    prefix = 'emu_' if emu else ''
    out = {}
    for addr, (label, width) in STATE.items():
        r = cmd(sock, f, f'{prefix}{"read_wram" if emu else "dump_ram"} 0x{addr:x} {width}')
        h = r['hex'].replace(' ', '')
        if emu:
            # emu_read_wram returns continuous hex string
            v = sum(int(h[2*i:2*i+2], 16) << (8*i) for i in range(width))
        else:
            # dump_ram returns space-separated per-byte little-endian
            bs = [int(h[2*i:2*i+2], 16) for i in range(width)]
            v = sum(b << (8*i) for i, b in enumerate(bs))
        out[label] = v
    return out


def main():
    subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.5)
    proc = subprocess.Popen([str(EXE), '--paused'], cwd=REPO,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        sock = socket.socket()
        for _ in range(50):
            try: sock.connect(('127.0.0.1', 4377)); break
            except (ConnectionRefusedError, OSError): time.sleep(0.2)
        f = sock.makefile('r'); f.readline()

        # Advance BOTH sides to mode 0x07
        MODE = 0x07
        for _ in range(2000):
            cmd(sock, f, 'step 1')
            r = cmd(sock, f, 'dump_ram 0x100 1')
            if int(r['hex'].replace(' ', ''), 16) == MODE: break
        for _ in range(2000):
            cmd(sock, f, 'emu_step 1')
            r = cmd(sock, f, 'emu_read_wram 0x100 1')
            if int(r['hex'].replace(' ', ''), 16) == MODE: break

        recomp_mode_frame = cmd(sock, f, 'frame').get('frame', 0)
        print(f'recomp reached mode 0x07 at frame {recomp_mode_frame}')

        # Step both sides in lockstep, compare state each frame.
        first_diverge = {}  # label -> (dwell_frame, oracle_val, recomp_val)
        for dwell in range(1, 300):
            cmd(sock, f, 'step 1')
            cmd(sock, f, 'emu_step 1')
            rs = read_state(sock, f, emu=False)
            os_ = read_state(sock, f, emu=True)
            for label in STATE.values():
                lbl = label[0]
                if rs[lbl] != os_[lbl] and lbl not in first_diverge:
                    first_diverge[lbl] = (dwell, os_[lbl], rs[lbl])
            # Exit early if game mode diverged (means Mario died)
            rmode = int(cmd(sock, f, 'dump_ram 0x100 1')['hex'].replace(' ', ''), 16)
            omode = int(cmd(sock, f, 'emu_read_wram 0x100 1')['hex'].replace(' ', ''), 16)
            if rmode != omode:
                print(f'GameMode diverged at dwell {dwell}: oracle=0x{omode:02x} recomp=0x{rmode:02x}')
                break

        print(f'\nFirst divergences (dwell = frames past mode 0x07 entry):')
        for lbl in sorted(first_diverge, key=lambda k: first_diverge[k][0]):
            dw, o, r = first_diverge[lbl]
            print(f'  dwell={dw:3} {lbl:28s} oracle=0x{o:04x} recomp=0x{r:04x}')
    finally:
        sock.close()
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill()
        subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


if __name__ == '__main__':
    sys.exit(main())
