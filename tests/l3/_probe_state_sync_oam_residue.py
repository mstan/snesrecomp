"""Trace which functions write $03E8/$14A2/etc. on each side during
boot. These are the upstream OAM/sprite-buffer divergences that
remain at GM=07 entry after the NMI-order fix.

RING-FIRST METHODOLOGY: queries the ALWAYS-ON WRAM trace via
`wram_writes_at` (recomp) and `emu_wram_writes_at` (oracle). Both
traces are armed at process startup, so writes from snes9x's
reset/boot AND recomp's I_RESET are recorded continuously. The
probe never resets traces — it queries history backward.
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

        # NO trace_wram_reset / trace_wram. Always-on traces are
        # already running. Just step both sides to GM=07 and then
        # query the rings.
        for _ in range(3000):
            cmd(sock, f, 'step 1')
            r = cmd(sock, f, 'dump_ram 0x100 1')['hex'].replace(' ', '')
            if int(r, 16) == 0x07: break
        for _ in range(3000):
            cmd(sock, f, 'emu_step 1')
            r = cmd(sock, f, 'emu_read_wram 0x100 1')['hex'].replace(' ', '')
            if int(r, 16) == 0x07: break

        for a in [0x3e8, 0x3eb, 0x3ec, 0x3ef, 0x41e, 0x49a, 0x49b,
                  0x14a2, 0x2142, 0x2143]:
            print(f'\n=== ${a:04x} ===')
            r = cmd(sock, f, f'wram_writes_at {a:x} 0 999999 32')
            matches = r.get('matches', [])
            print(f'  rec: {len(matches)} writes (limit 32)')
            for e in matches:
                print(f'    f={e["f"]} val={e["val"]} w={e["w"]} '
                      f'func={e["func"]} parent={e["parent"]}')
            er = cmd(sock, f, f'emu_wram_writes_at {a:x} 0 999999 32')
            ematches = er.get('matches', [])
            print(f'  emu: {len(ematches)} writes (limit 32)')
            for e in ematches:
                print(f'    f={e["f"]} pc={e["pc"]} '
                      f'{e["before"]}->{e["after"]} bank={e["bank_src"]}')
    finally:
        try: sock.close()
        except Exception: pass
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill()
        _kill()


if __name__ == '__main__':
    main()
