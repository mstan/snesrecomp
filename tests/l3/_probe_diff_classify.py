"""Triage the remaining 500-byte diff at GM=07 sync. Classify
each diff by its WRAM region per SMW's rammap so we can see
WHAT KINDS of state are diverging — that points at WHICH
subsystem (sprite-render, sound, PPU mirror, etc.) is the next
thing to fix.
"""
from __future__ import annotations
import json, pathlib, socket, subprocess, time

REPO = pathlib.Path(__file__).parent.parent.parent.parent
EXE = REPO / 'build' / 'bin-x64-Oracle' / 'smw.exe'
PORT = 4377


def _kill():
    subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def cmd(sock, f, line):
    sock.sendall((line + '\n').encode())
    return json.loads(f.readline())


def _read_range(sock, f, side, addr, length):
    out = bytearray()
    while length > 0:
        n = min(1024, length)
        c = (f'dump_ram 0x{addr:x} {n}' if side == 'rec'
             else f'emu_read_wram 0x{addr:x} {n}')
        r = cmd(sock, f, c)
        out.extend(bytes.fromhex(r.get('hex', '').replace(' ', '')))
        addr += n; length -= n
    return bytes(out)


# Region map for SMW WRAM (from rammap.asm summary). Address ranges
# carved by what SMW USES the bytes for; lets us see "47 sprite-table
# bytes diverge" instead of "47 random addresses diverge."
REGIONS = [
    (0x0000, 0x001F, 'DP_scratch (filtered out)'),
    (0x0020, 0x006F, 'DP_game_state (player + level)'),
    (0x0070, 0x008F, 'DP_player_collision'),
    (0x0090, 0x00FF, 'DP_misc'),
    (0x0100, 0x01FF, 'stack_page (filtered out)'),
    (0x0200, 0x03FF, 'OAM_mirror_lo (XYTI per sprite)'),
    (0x0400, 0x041F, 'OAM_tile_size_bits'),
    (0x0420, 0x049F, 'OAM_tile_size_full'),
    (0x04A0, 0x06FF, 'CGRAM/HDMA buffers'),
    (0x0700, 0x07FF, 'palette / GFX scratch'),
    (0x0800, 0x0BFF, 'GFX decompress buffers'),
    (0x0C00, 0x0CFF, 'palette tables'),
    (0x0D00, 0x0D5F, 'credits/cutscene sprites'),
    (0x0D60, 0x0D9F, 'BounceSprite/QuakeSprite/ScoreSprite'),
    (0x0DA0, 0x0DFF, 'ControllersPresent + SPC mirrors'),
    (0x0E00, 0x0FFF, 'Mode 7 / VRAM addresses'),
    (0x1000, 0x13FF, 'level data / GFX state'),
    (0x1400, 0x14FF, 'sprite engine state'),
    (0x1500, 0x16FF, 'sprite tables'),
    (0x1700, 0x18FF, 'sprite extended'),
    (0x1900, 0x1AFF, 'level rendering / blocks'),
    (0x1B00, 0x1DFF, 'misc level state + SPC IO'),
    (0x1E00, 0x1FFF, 'TitleInputIndex/timers/SPCIO mirrors'),
    (0x2000, 0x7FFF, 'extended_state (level/HDMA)'),
    (0x8000, 0xFFFF, 'WRAM bank 7E mirror+'),
    (0x10000, 0x1FFFF, 'WRAM bank 7F'),
]


def classify(addr):
    for lo, hi, name in REGIONS:
        if lo <= addr <= hi:
            return name
    return f'unknown@{addr:05x}'


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

        # Step ONLY rec; emu auto-advances via emu_oracle_run_frame.
        rec_steps = 0
        for _ in range(3000):
            cmd(sock, f, 'step 1'); rec_steps += 1
            gm = cmd(sock, f, 'dump_ram 0x100 1')['hex'].replace(' ', '')
            if int(gm, 16) == 0x07: break
        emu_gm = cmd(sock, f, 'emu_read_wram 0x100 1')['hex'].replace(' ', '')
        print(f'[at lockstep GM=07-rec] rec_steps={rec_steps} '
              f'emu_$0100=${int(emu_gm, 16):02x}')

        rec = _read_range(sock, f, 'rec', 0x0000, 0x20000)
        emu = _read_range(sock, f, 'emu', 0x0000, 0x20000)

        from collections import Counter
        by_region = Counter()
        diffs_by_region = {}
        # Init-policy noise: snes9x fills uninitialized WRAM with
        # 0x55 at power-on; recomp's BSS-zero leaves it at 0x00.
        # SMW's reset code zeros bank-7E:$0000-$1FFF on both sides,
        # but $2000-$FFFF and bank 7F stay at the init fill. These
        # bytes aren't read by SMW under normal play; the diff is
        # init-convention only, not a real divergence.
        init_policy = 0
        for i in range(0x20000):
            if rec[i] == emu[i]: continue
            if rec[i] == 0x00 and emu[i] == 0x55:
                init_policy += 1
                continue
            r = classify(i)
            if 'filtered' in r: continue
            by_region[r] += 1
            diffs_by_region.setdefault(r, []).append((i, rec[i], emu[i]))

        total = sum(by_region.values())
        print(f'\n[init-policy noise (rec=$00 / emu=$55) excluded]: {init_policy} bytes')
        print(f'[real diffs] total meaningful: {total}')
        for region, count in by_region.most_common():
            sample = diffs_by_region[region][:5]
            sample_str = ' '.join(f'${a:04x}=({r:02x}/{e:02x})'
                                  for a, r, e in sample)
            print(f'  {count:5d}  {region}')
            print(f'           sample: {sample_str}')
    finally:
        try: sock.close()
        except Exception: pass
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill()
        _kill()


if __name__ == '__main__':
    main()
