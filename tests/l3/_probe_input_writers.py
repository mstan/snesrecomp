"""Trace writers to $15-$18 (joypad shadow regs) on both sides
starting at GM=07 entry. Recomp shows $15/$16=$00 throughout the
demo while emu shows real input bytes — find which functions write
those slots on emu but NOT on recomp."""
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


def _read_byte(sock, f, addr, side):
    if side == 'rec':
        r = cmd(sock, f, f'dump_ram 0x{addr:x} 1')
    else:
        r = cmd(sock, f, f'emu_read_wram 0x{addr:x} 1')
    h = r.get('hex', '').replace(' ', '')
    return int(h, 16) if h else -1


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

        # GM=07 sync each side independently.
        for _ in range(3000):
            cmd(sock, f, 'step 1')
            if _read_byte(sock, f, 0x100, 'rec') == 0x07: break
        for _ in range(3000):
            cmd(sock, f, 'emu_step 1')
            if _read_byte(sock, f, 0x100, 'emu') == 0x07: break

        # Arm trace on $15-$18 (joypad shadows) + the underlying
        # WRAM mirrors $0DA2-$0DAB (controller raw mirrors) +
        # $0DA0 (ControllersPresent).
        addrs = '15 16 17 18 0da2 0da3 0da4 0da5 0da6 0da7 0da8 0da9 0daa'
        cmd(sock, f, 'trace_wram_reset')
        cmd(sock, f, f'trace_wram {addrs}')
        cmd(sock, f, 'emu_wram_trace_reset')
        cmd(sock, f, f'emu_wram_trace_add {addrs}')

        # Step 30 frames in lockstep.
        for _ in range(30):
            cmd(sock, f, 'step 1'); cmd(sock, f, 'emu_step 1')

        # Sample current state.
        print('Current $15-$18 (post 30 frames at GM=07):')
        for a in (0x15, 0x16, 0x17, 0x18):
            print(f'  ${a:02x}: rec=${_read_byte(sock, f, a, "rec"):02x}, '
                  f'emu=${_read_byte(sock, f, a, "emu"):02x}')

        rt = cmd(sock, f, 'get_wram_trace').get('log', [])
        et = cmd(sock, f, 'emu_get_wram_trace').get('log', [])

        # Group recomp writes by adr+func, count frequency.
        from collections import Counter
        rec_counts = Counter()
        rec_funcs = {}
        for e in rt:
            key = (e.get('adr'), e.get('func'))
            rec_counts[key] += 1
            rec_funcs[e.get('adr')] = rec_funcs.get(e.get('adr'), set())
            rec_funcs[e.get('adr')].add(e.get('func'))

        emu_counts = Counter()
        emu_pcs = {}
        for e in et:
            key = (e.get('adr'), e.get('pc'))
            emu_counts[key] += 1
            emu_pcs[e.get('adr')] = emu_pcs.get(e.get('adr'), set())
            emu_pcs[e.get('adr')].add(e.get('pc'))

        print(f'\n=== RECOMP writers (total {len(rt)} events) ===')
        for (adr, func), n in sorted(rec_counts.items()):
            print(f'  {adr} x{n:3d}  func={func}')

        print(f'\n=== EMU writers (total {len(et)} events) ===')
        for (adr, pc), n in sorted(emu_counts.items()):
            print(f'  {adr} x{n:3d}  pc={pc}')

        # Per-address summary: addresses present on emu but not recomp.
        rec_adrs = set(rec_funcs.keys())
        emu_adrs = set(emu_pcs.keys())
        only_emu = emu_adrs - rec_adrs
        only_rec = rec_adrs - emu_adrs
        print(f'\n[diff] addrs ONLY emu: {sorted(only_emu)}')
        print(f'[diff] addrs ONLY rec: {sorted(only_rec)}')
    finally:
        try: sock.close()
        except Exception: pass
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill()
        _kill()


if __name__ == '__main__':
    main()
