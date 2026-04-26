"""Full byte-level diff at GM=07 sync, listing every diverging byte
in the game-state range $0020-$1FFF. With NMI-order fix landed
and DP labeled state matching, only 8 bytes diverge in $0200-$1FFF
and 2 bytes in $2000-$7FFF — list them all to identify if any are
load-bearing for demo timing (TitleInputIndex/VariousPromptTimer)."""
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
    if side == 'rec':
        chunks = []
        while length > 0:
            n = min(1024, length)
            r = cmd(sock, f, f'dump_ram 0x{addr:x} {n}')
            hexs = r.get('hex', '').replace(' ', '')
            chunks.append(bytes.fromhex(hexs))
            addr += n; length -= n
        return b''.join(chunks)
    else:
        chunks = []
        while length > 0:
            n = min(1024, length)
            r = cmd(sock, f, f'emu_read_wram 0x{addr:x} {n}')
            hexs = r.get('hex', '').replace(' ', '')
            chunks.append(bytes.fromhex(hexs))
            addr += n; length -= n
        return b''.join(chunks)


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
            r = cmd(sock, f, 'dump_ram 0x100 1')['hex'].replace(' ', '')
            if int(r, 16) == 0x07: break
        for _ in range(3000):
            cmd(sock, f, 'emu_step 1')
            r = cmd(sock, f, 'emu_read_wram 0x100 1')['hex'].replace(' ', '')
            if int(r, 16) == 0x07: break

        # Read full $0000-$7FFF on each side.
        rec = _read_range(sock, f, 'rec', 0x0000, 0x8000)
        emu = _read_range(sock, f, 'emu', 0x0000, 0x8000)

        diffs = []
        for i in range(0x8000):
            if rec[i] != emu[i]:
                diffs.append((i, rec[i], emu[i]))

        # Skip noise: $0000-$001F (DP scratch), $0100-$01FF (stack).
        meaningful = [(a, r, e) for a, r, e in diffs
                      if not (a < 0x20 or 0x100 <= a < 0x200)]

        print(f'Total raw diffs: {len(diffs)}')
        print(f'Diffs excluding DP scratch + stack: {len(meaningful)}')
        for a, r, e in meaningful:
            note = ''
            if 0x1F49 <= a <= 0x1F4F: note = ' (TitleInputIndex/VariousPromptTimer area?)'
            print(f'  ${a:04x}: rec=${r:02x} emu=${e:02x}{note}')
    finally:
        try: sock.close()
        except Exception: pass
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill()
        _kill()


if __name__ == '__main__':
    main()
