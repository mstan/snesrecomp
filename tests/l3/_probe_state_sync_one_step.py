"""After GM=07 sync (10 meaningful diffs only), step both sides
1 frame and re-diff. New divergences in 1 step name a per-frame
codegen issue. Persistent ones from GM=07 entry are uninitialized
memory residue."""
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


def diff_set(rec, emu):
    s = set()
    for i in range(min(len(rec), len(emu))):
        if rec[i] != emu[i]:
            if i < 0x20 or 0x100 <= i < 0x200:
                continue  # skip noise
            s.add(i)
    return s


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

        # Snapshot at +0
        rec0 = _read_range(sock, f, 'rec', 0x0000, 0x8000)
        emu0 = _read_range(sock, f, 'emu', 0x0000, 0x8000)
        d0 = diff_set(rec0, emu0)
        print(f'[+0 GM=07 entry] meaningful diffs: {len(d0)}')

        # Step 1 frame in lockstep, re-diff.
        for step in range(1, 11):
            cmd(sock, f, 'step 1'); cmd(sock, f, 'emu_step 1')
            rec = _read_range(sock, f, 'rec', 0x0000, 0x8000)
            emu = _read_range(sock, f, 'emu', 0x0000, 0x8000)
            d = diff_set(rec, emu)
            new = d - d0
            gone = d0 - d
            print(f'[+{step}] total diffs: {len(d)} | '
                  f'NEW since +0: {len(new)} | gone: {len(gone)}')
            if step == 1 and new:
                print(f'  first 30 NEW diff addrs:')
                for a in sorted(new)[:30]:
                    print(f'    ${a:04x}: rec=${rec[a]:02x} emu=${emu[a]:02x} '
                          f'(was rec=${rec0[a]:02x} emu=${emu0[a]:02x})')
            d0 = d  # baseline rolls forward
    finally:
        try: sock.close()
        except Exception: pass
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill()
        _kill()


if __name__ == '__main__':
    main()
