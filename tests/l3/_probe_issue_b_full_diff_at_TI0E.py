"""Issue B: full WRAM diff at TI=$0E (the last-sync moment).

Mario fields match at TI=$0E but diverge by TI=$10. So some
non-Mario WRAM byte differs at TI=$0E and propagates over the
128-frame interval to corrupt Mario state.

This probe:
  1. Step both sides until they have TI=$0E in their history.
  2. Look up rec_frame and emu_frame for TI=$0E from the $1DF4
     write trace.
  3. Diff every byte in $0000-$1FFF on rec (via dump_frame_wram)
     vs emu (via emu_wram_at_frame).
  4. Filter init-policy noise (rec=$00 / emu=$55).
  5. List every diff. Group by SMW region.
"""
from __future__ import annotations
import json, pathlib, socket, subprocess, time
from collections import Counter

REPO = pathlib.Path(__file__).parent.parent.parent.parent
EXE = REPO / 'build' / 'bin-x64-Oracle' / 'smw.exe'
PORT = 4377


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


REGIONS = [
    (0x0000, 0x001F, 'DP_scratch'),
    (0x0020, 0x006F, 'DP_game_state'),
    (0x0070, 0x008F, 'DP_player_collision'),
    (0x0090, 0x00FF, 'DP_misc'),
    (0x0100, 0x01FF, 'stack_page'),
    (0x0200, 0x03FF, 'OAM_mirror_lo'),
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
]


def classify(addr):
    for lo, hi, name in REGIONS:
        if lo <= addr <= hi:
            return name
    return f'<{addr:05x}>'


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

        for _ in range(3000):
            cmd(sock, f, 'step 1')
            if _read_byte(sock, f, 0x100, 'rec') == 0x07: break
        for _ in range(800):
            cmd(sock, f, 'step 1')

        # Locate rec's TI=$0E frame from the always-on write trace.
        rec_writes = cmd(sock, f, 'wram_writes_at 1df4 0 999999 4096').get('matches', [])
        rec_f = next((int(e['f']) for e in rec_writes
                      if int(e['val'], 16) & 0xFF == 0x0E), None)
        if rec_f is None:
            print('rec TI=$0E not found in $1df4 trace'); return

        # emu's WRAM watch ring evicts old entries, so use the
        # per-frame snapshot ring instead. Walk emu's history range
        # and read $1df4 at each frame; find the frame where it
        # equals $0E.
        h = cmd(sock, f, 'emu_history')
        oldest = h.get('oldest', -1)
        newest = h.get('newest', -1)
        print(f'emu history range: {oldest}..{newest} ({h.get("count")} frames)')

        emu_f = None
        # Binary-walk: $1df4 monotonically increases in the demo;
        # find the first frame where it == $0E.
        for fi in range(oldest, newest + 1):
            r = cmd(sock, f, f'emu_wram_at_frame {fi} 1df4')
            if r.get('ok') and int(r['val'], 16) == 0x0E:
                emu_f = fi
                break
        if emu_f is None:
            # TI=$0E may be before the history window. Walk forward
            # from oldest, find smallest fi where val > 0x0E and
            # report the gap.
            print('emu TI=$0E not in history; checking history bounds...')
            r0 = cmd(sock, f, f'emu_wram_at_frame {oldest} 1df4')
            r1 = cmd(sock, f, f'emu_wram_at_frame {newest} 1df4')
            print(f'  $1df4 at oldest history frame {oldest}: '
                  f'${int(r0.get("val", "0x0"), 16):02x}')
            print(f'  $1df4 at newest history frame {newest}: '
                  f'${int(r1.get("val", "0x0"), 16):02x}')
            return
        print(f'TI=$0E at: rec_frame={rec_f}, emu_frame={emu_f}')

        # Pull bank-7E low WRAM ($0000-$1FFF) from each side's
        # historical snapshot. Use chunked reads.
        def pull_rec_chunk(addr, length):
            r = cmd(sock, f, f'dump_frame_wram {rec_f} {addr:x} {length}')
            hex_str = r.get('hex', '').replace(' ', '')
            return bytes.fromhex(hex_str)

        # emu side: query each byte individually since
        # emu_wram_at_frame is per-byte. 8KB = 8192 round-trips.
        # That's slow but ok one-shot. Actually the JSON cap in the
        # query is by-byte; let me just do all 8KB.
        print(f'\nreading rec WRAM[$0000-$1FFF] from frame {rec_f} ...')
        rec_bytes = bytearray(0x2000)
        chunk = 1024
        for off in range(0, 0x2000, chunk):
            data = pull_rec_chunk(off, chunk)
            rec_bytes[off:off+len(data)] = data
        print(f'reading emu WRAM[$0000-$1FFF] from frame {emu_f} ...')
        emu_bytes = bytearray(0x2000)
        for off in range(0x2000):
            r = cmd(sock, f, f'emu_wram_at_frame {emu_f} {off:x}')
            if r.get('ok'):
                emu_bytes[off] = int(r['val'], 16)
            else:
                emu_bytes[off] = 0xff  # unreadable

        # Diff (skip stack page).
        diffs = []
        init_policy = 0
        for i in range(0x2000):
            if rec_bytes[i] == emu_bytes[i]:
                continue
            if 0x100 <= i < 0x200:
                continue  # skip stack page
            if rec_bytes[i] == 0x00 and emu_bytes[i] == 0x55:
                init_policy += 1
                continue
            diffs.append((i, rec_bytes[i], emu_bytes[i]))

        by_region = Counter()
        for a, r, e in diffs:
            by_region[classify(a)] += 1

        print(f'\n[init-policy noise: {init_policy} bytes excluded]')
        print(f'[real diffs at TI=$0E: {len(diffs)} bytes]')
        for region, n in by_region.most_common():
            samples = [(a, r, e) for a, r, e in diffs if classify(a) == region][:6]
            sample_str = ' '.join(f'${a:04x}({r:02x}/{e:02x})' for a, r, e in samples)
            print(f'  {n:5}  {region}')
            print(f'         {sample_str}')

        # First 30 diffs in address order.
        print(f'\nFirst 40 diff bytes by address:')
        for a, r, e in diffs[:40]:
            print(f'  ${a:04x}: rec=${r:02x} emu=${e:02x}  ({classify(a)})')
    finally:
        try: sock.close()
        except Exception: pass
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill()
        _kill()


if __name__ == '__main__':
    main()
