"""Print chronological order of $15/$16 writes within ONE frame at
GM=07 on each side. Recomp ends at $00, emu ends at $C1 — so either
the recomp's writes happen in a different order, or the values
written differ."""
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

        for _ in range(3000):
            cmd(sock, f, 'step 1')
            if _read_byte(sock, f, 0x100, 'rec') == 0x07: break
        for _ in range(3000):
            cmd(sock, f, 'emu_step 1')
            if _read_byte(sock, f, 0x100, 'emu') == 0x07: break

        # Step 5 frames in lockstep so we're past first transitions.
        for _ in range(5):
            cmd(sock, f, 'step 1'); cmd(sock, f, 'emu_step 1')

        # Arm fresh and step ONE frame so we get the writes for
        # exactly one frame.
        cmd(sock, f, 'trace_wram_reset')
        cmd(sock, f, 'trace_wram 15 16 17 18')
        cmd(sock, f, 'emu_wram_trace_reset')
        cmd(sock, f, 'emu_wram_trace_add 15 16 17 18')

        cmd(sock, f, 'step 1'); cmd(sock, f, 'emu_step 1')

        rt = cmd(sock, f, 'get_wram_trace').get('log', [])
        et = cmd(sock, f, 'emu_get_wram_trace').get('log', [])

        print(f'=== RECOMP $15/$16 chronological writes ({len(rt)} events) ===')
        for e in rt:
            print(f'  adr={e.get("adr")} val={e.get("val")} '
                  f'w={e.get("w")} func={e.get("func")} '
                  f'parent={e.get("parent")}')

        print(f'\n=== EMU $15/$16 chronological writes ({len(et)} events) ===')
        for e in et:
            print(f'  adr={e.get("adr")} pc={e.get("pc")} '
                  f'{e.get("before")}->{e.get("after")}')

        # Final values.
        print(f'\nFinal: rec $15=${_read_byte(sock, f, 0x15, "rec"):02x} '
              f'$16=${_read_byte(sock, f, 0x16, "rec"):02x} | '
              f'emu $15=${_read_byte(sock, f, 0x15, "emu"):02x} '
              f'$16=${_read_byte(sock, f, 0x16, "emu"):02x}')
    finally:
        try: sock.close()
        except Exception: pass
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill()
        _kill()


if __name__ == '__main__':
    main()
