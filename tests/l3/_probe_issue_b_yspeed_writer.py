"""Issue B follow-up: capture the writer to player Y-speed ($7D) on
the EXACT frame the recomp diverges from oracle.

Phase 1 (`_probe_issue_b_mario_y.py`) found:
  * Y-state matches at GM=07 sync.
  * +5,+6,+7 frames: rec/emu both have $7D=$06.
  * +8: rec $7D=$06 (still walking-fall) but emu $7D=$AE (jumping).

So somewhere on frame +8 the emu writes $7D=$AE and the recomp does
not. To find the writer: arm trace_wram($7D) on BOTH sides at frame
+7, then step 1, then dump traces.
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

        # Both sides → GM=07.
        rf = ef = 0
        for _ in range(3000):
            cmd(sock, f, 'step 1'); rf += 1
            if _read_byte(sock, f, 0x0100, 'rec') == 0x07: break
        for _ in range(3000):
            cmd(sock, f, 'emu_step 1'); ef += 1
            if _read_byte(sock, f, 0x0100, 'emu') == 0x07: break
        print(f'[init] rec→GM07 in {rf} frames, emu→GM07 in {ef} frames')

        # Step both 7 frames into gameplay.
        for _ in range(7):
            cmd(sock, f, 'step 1'); cmd(sock, f, 'emu_step 1')

        # Confirm we're at the pre-divergence sync point.
        rs = _read_byte(sock, f, 0x7d, 'rec')
        es = _read_byte(sock, f, 0x7d, 'emu')
        print(f'[+7 frames] rec $7D=${rs:02x}, emu $7D=${es:02x}  '
              f'(expect both $06)')
        if rs != es:
            print('  WARN: $7D already differs — bug is earlier than +7')

        # Arm trace on $7D specifically (and $96/$97 + $D3/$D4 for
        # context).
        cmd(sock, f, 'trace_wram_reset')
        cmd(sock, f, 'trace_wram 7d 96 97 d3 d4')
        cmd(sock, f, 'emu_wram_trace_reset')
        cmd(sock, f, 'emu_wram_trace_add 7d 96 97 d3 d4')

        # Step 1 frame on both — this is the divergent frame.
        cmd(sock, f, 'step 1'); cmd(sock, f, 'emu_step 1')

        # Read post-step value to confirm divergence happened in
        # this window.
        rs2 = _read_byte(sock, f, 0x7d, 'rec')
        es2 = _read_byte(sock, f, 0x7d, 'emu')
        print(f'[+8 frames] rec $7D=${rs2:02x}, emu $7D=${es2:02x}  '
              f'(expect rec $06 / emu $ae)')

        rt = cmd(sock, f, 'get_wram_trace').get('log', [])
        et = cmd(sock, f, 'emu_get_wram_trace').get('log', [])

        print(f'\n[recomp writers] {len(rt)} events @ +8 frame:')
        for e in rt:
            print(f'  adr={e.get("adr")} val={e.get("val")} '
                  f'w={e.get("w")} func={e.get("func")} <- '
                  f'parent={e.get("parent")}')

        print(f'\n[emu writers] {len(et)} events @ +8 frame:')
        for e in et:
            print(f'  adr={e.get("adr")} pc={e.get("pc")} '
                  f'{e.get("before")}→{e.get("after")} '
                  f'bank={e.get("bank_src")}')

        # Identify the diverging $7D write specifically.
        rec_7d = [e for e in rt if e.get('adr', '').endswith('07d')
                  or e.get('adr') == '0x0007d']
        emu_7d = [e for e in et if e.get('adr', '').endswith('07d')
                  or e.get('adr') == '0x0007d']
        print(f'\n[$7D writes] recomp: {len(rec_7d)}, emu: {len(emu_7d)}')
        for e in rec_7d:
            print(f'  REC: val={e.get("val")} func={e.get("func")} '
                  f'parent={e.get("parent")}')
        for e in emu_7d:
            print(f'  EMU: pc={e.get("pc")} {e.get("before")}→'
                  f'{e.get("after")}')
    finally:
        try: sock.close()
        except Exception: pass
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill()
        _kill()


if __name__ == '__main__':
    main()
