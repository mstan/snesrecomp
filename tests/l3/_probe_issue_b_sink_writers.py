"""Issue B phase 5: capture Mario Y writers across the sink event.

Phase 4 found:
  +270: rY=$014F (1 px above ground)
  +275: rY=$0160 (1 tile under ground — Issue B reproduced)
  Mario stuck at X=$01C7 / Y=$0160 from +275 onward.

To find the bug: arm trace_wram on $D3/$D4 (player Y position word)
and $96/$97 (player Y next word) at frame +270, then step to frame
+278 capturing every writer. Diff recomp vs emu writer chains.

The first divergent write to $D3/$D4 names the buggy code path.
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


def _read_byte(sock, f, addr, side):
    if side == 'rec':
        r = cmd(sock, f, f'dump_ram 0x{addr:x} 1')
    else:
        r = cmd(sock, f, f'emu_read_wram 0x{addr:x} 1')
    h = r.get('hex', '').replace(' ', '')
    return int(h, 16) if h else -1


def _read_word(sock, f, addr, side):
    lo = _read_byte(sock, f, addr, side)
    hi = _read_byte(sock, f, addr + 1, side)
    return (hi << 8) | lo


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

        # GM=07 sync
        for _ in range(3000):
            cmd(sock, f, 'step 1')
            if _read_byte(sock, f, 0x100, 'rec') == 0x07: break
        for _ in range(3000):
            cmd(sock, f, 'emu_step 1')
            if _read_byte(sock, f, 0x100, 'emu') == 0x07: break

        # Step to +270 (just before sink event).
        for _ in range(270):
            cmd(sock, f, 'step 1'); cmd(sock, f, 'emu_step 1')

        rx = _read_word(sock, f, 0xD1, 'rec')
        ry = _read_word(sock, f, 0xD3, 'rec')
        ex = _read_word(sock, f, 0xD1, 'emu')
        ey = _read_word(sock, f, 0xD3, 'emu')
        print(f'[+270] rec X=${rx:04x} Y=${ry:04x}  '
              f'emu X=${ex:04x} Y=${ey:04x}')

        # Arm trace on Mario Y bytes + collision-related state on
        # both sides. $D3/$D4 = Y position. $96/$97 = Y next.
        # $80/$81 = collision flags. $1471 = block-hit flag.
        # $7D = Y velocity.
        addrs = '7d 96 97 d3 d4 80 81 1471 1472'
        cmd(sock, f, 'trace_wram_reset')
        cmd(sock, f, f'trace_wram {addrs}')
        cmd(sock, f, 'emu_wram_trace_reset')
        cmd(sock, f, f'emu_wram_trace_add {addrs}')

        # Step the sink window — 8 frames covers +270 → +278.
        for _ in range(8):
            cmd(sock, f, 'step 1'); cmd(sock, f, 'emu_step 1')

        ry2 = _read_word(sock, f, 0xD3, 'rec')
        ey2 = _read_word(sock, f, 0xD3, 'emu')
        print(f'[+278] rec Y=${ry2:04x}  emu Y=${ey2:04x}  '
              f'(expect rec $0160, emu $0150)')

        rt = cmd(sock, f, 'get_wram_trace').get('log', [])
        et = cmd(sock, f, 'emu_get_wram_trace').get('log', [])
        print(f'\n[recomp writers] {len(rt)} events:')
        for e in rt:
            print(f'  adr={e.get("adr")} val={e.get("val")} '
                  f'w={e.get("w")} f={e.get("func")} <- '
                  f'p={e.get("parent")}')
        print(f'\n[emu writers] {len(et)} events:')
        for e in et:
            print(f'  adr={e.get("adr")} pc={e.get("pc")} '
                  f'{e.get("before")}->{e.get("after")} '
                  f'bank={e.get("bank_src")}')

        # Pull out the $D3/$D4 writes specifically, on both sides.
        rec_y = [e for e in rt if e.get('adr') in ('0x000d3', '0x000d4')]
        emu_y = [e for e in et if e.get('adr') in ('0x000d3', '0x000d4')]
        print(f'\n[$D3/$D4 writes] recomp: {len(rec_y)}, emu: {len(emu_y)}')
        for e in rec_y:
            print(f'  REC: adr={e["adr"]} val={e["val"]} '
                  f'f={e.get("func")} p={e.get("parent")}')
        for e in emu_y:
            print(f'  EMU: adr={e["adr"]} pc={e["pc"]} '
                  f'{e["before"]}->{e["after"]}')
    finally:
        try: sock.close()
        except Exception: pass
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill()
        _kill()


if __name__ == '__main__':
    main()
