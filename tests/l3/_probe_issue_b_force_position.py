"""Issue B: force-write Mario's position on both sides to bypass
demo desync, then step 1 frame to see whether physics produces
different Y on rec vs emu.

Strategy:
  1. Step both into gameplay (GM=$14 area).
  2. Pause; force-write Mario's position fields on both sides:
     $94/$95 PlayerXPosNext (X), $96/$97 PlayerYPosNext (Y),
     $D1/$D2 PlayerXPos, $D3/$D4 PlayerYPos, $7B XSpeed,
     $7D YSpeed.
  3. Step both 1 frame in lockstep.
  4. Compare Mario state.
  5. Repeat: step 1, compare, etc., until divergence appears.

If both sides start from identical Mario state and diverge in
N frames, the divergence IS in deterministic per-frame code —
real Issue B bug, with attribution from the always-on rings
narrowed to the call chain in those N frames.
"""
from __future__ import annotations
import json, pathlib, socket, subprocess, sys, time

REPO = pathlib.Path(__file__).parent.parent.parent.parent
EXE = REPO / 'build' / 'bin-x64-Oracle' / 'smw.exe'
PORT = 4377

# Force Mario to a position that earlier probes saw as a sink site.
TARGET_X = 0x01C7
TARGET_Y = 0x0140    # in air, just above ground; let physics drop him
TARGET_XSPD = 0      # at rest horizontally
TARGET_YSPD = 0      # initial Y velocity 0


def _kill():
    subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def cmd(sock, f, line):
    sock.sendall((line + '\n').encode())
    return json.loads(f.readline())


def _read_byte(sock, f, addr, side):
    c = (f'dump_ram 0x{addr:x} 1' if side == 'rec'
         else f'emu_read_wram 0x{addr:x} 1')
    h = cmd(sock, f, c).get('hex', '').replace(' ', '')
    return int(h, 16) if h else -1


def _read_word(sock, f, addr, side):
    return (_read_byte(sock, f, addr + 1, side) << 8) | _read_byte(sock, f, addr, side)


def _write_byte(sock, f, addr, val, side):
    if side == 'rec':
        c = f'write_ram 0x{addr:x} {val:02x}'
    else:
        c = f'emu_write_wram 0x{addr:x} {val:02x}'
    return cmd(sock, f, c)


def _write_word(sock, f, addr, val, side):
    _write_byte(sock, f, addr, val & 0xFF, side)
    _write_byte(sock, f, addr + 1, (val >> 8) & 0xFF, side)


def force_mario(sock, f, side):
    _write_word(sock, f, 0x94, TARGET_X, side)
    _write_word(sock, f, 0x96, TARGET_Y, side)
    _write_word(sock, f, 0xD1, TARGET_X, side)
    _write_word(sock, f, 0xD3, TARGET_Y, side)
    _write_byte(sock, f, 0x7B, TARGET_XSPD & 0xFF, side)
    _write_byte(sock, f, 0x7D, TARGET_YSPD & 0xFF, side)


def force_no_input(sock, f, side):
    """Zero out joypad shadow registers so Mario gets no input."""
    for addr in (0x15, 0x16, 0x17, 0x18):
        _write_byte(sock, f, addr, 0, side)


def main():
    _kill(); time.sleep(0.5)
    proc = subprocess.Popen([str(EXE), '--paused'], cwd=str(REPO),
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    try:
        sock = socket.socket()
        for _ in range(60):
            try:
                sock.connect(('127.0.0.1', PORT)); break
            except (ConnectionRefusedError, OSError):
                time.sleep(0.2)
        f = sock.makefile('r'); f.readline()
        cmd(sock, f, 'pause')

        # Step into gameplay.
        for _ in range(3000):
            cmd(sock, f, 'step 1')
            if _read_byte(sock, f, 0x100, 'rec') == 0x07: break
        for _ in range(80):
            cmd(sock, f, 'step 1')

        rec_gm = _read_byte(sock, f, 0x100, 'rec')
        emu_gm = _read_byte(sock, f, 0x100, 'emu')
        print(f'pre-force: rec_GM=${rec_gm:02x} emu_GM=${emu_gm:02x}')

        # Confirm both sides have a level loaded (LevelMode_1422).
        rec_lm = _read_byte(sock, f, 0x1422, 'rec')
        emu_lm = _read_byte(sock, f, 0x1422, 'emu')
        print(f'pre-force: rec_LevelMode=${rec_lm:02x} emu_LevelMode=${emu_lm:02x}')

        # Force GM=$14 (level mode) to bypass title-screen demo
        # override that overwrites $15/$16 with scripted input
        # bytes. In GM=$14, joypad shadows reflect REAL input
        # (which is zero — no controller plugged in for either
        # side).
        _write_byte(sock, f, 0x100, 0x14, 'rec')
        _write_byte(sock, f, 0x100, 0x14, 'emu')

        # Force identical Mario state on both sides.
        force_mario(sock, f, 'rec')
        force_mario(sock, f, 'emu')

        # Verify the force took effect.
        for label, side in [('rec', 'rec'), ('emu', 'emu')]:
            x = _read_word(sock, f, 0xD1, side)
            y = _read_word(sock, f, 0xD3, side)
            xn = _read_word(sock, f, 0x94, side)
            yn = _read_word(sock, f, 0x96, side)
            ys = _read_byte(sock, f, 0x7D, side)
            print(f'post-force {label}: X=${x:04x} Y=${y:04x}  '
                  f'XNext=${xn:04x} YNext=${yn:04x}  YSpd=${ys:02x}')

        # Lockstep step. Each step = 1 logical frame on both sides
        # (per the yield-on-NMI architecture). Re-force inputs to 0
        # AND re-force position before each step so the demo override
        # can't drift Mario apart on its own.
        print(f'\nlockstep (re-force position + zero inputs each step):')
        print('step | rec X    Y    YS  | emu X    Y    YS  | div')
        prev_div = None
        for fi in range(1, 30):
            # Force-clear inputs each frame so demo override doesn't
            # win.
            force_no_input(sock, f, 'rec')
            force_no_input(sock, f, 'emu')
            cmd(sock, f, 'step 1')
            rx = _read_word(sock, f, 0xD1, 'rec')
            ry = _read_word(sock, f, 0xD3, 'rec')
            rs = _read_byte(sock, f, 0x7D, 'rec')
            ex = _read_word(sock, f, 0xD1, 'emu')
            ey = _read_word(sock, f, 0xD3, 'emu')
            es = _read_byte(sock, f, 0x7D, 'emu')
            div = ''
            if rx != ex or ry != ey or rs != es:
                div = '<-- DIVERGE'
                if prev_div is None:
                    prev_div = fi
            print(f'{fi:4} | ${rx:04x} ${ry:04x} ${rs:02x} | '
                  f'${ex:04x} ${ey:04x} ${es:02x}  {div}')
        if prev_div is not None:
            print(f'\nfirst divergence at step {prev_div}')
        else:
            print(f'\nno divergence in 30 lockstep steps from forced position')
    finally:
        try: sock.close()
        except Exception: pass
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill()
        _kill()


if __name__ == '__main__':
    main()
