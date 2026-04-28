"""From phase-pinned sync, lockstep step both sides 1 frame at a
time and find the FIRST frame where any meaningful WRAM byte
diverges. Reports the address + values at the divergent moment.

This isolates "what state actually drives the per-frame divergence
between recomp and snes9x" once demo-phase noise is eliminated.
"""
from __future__ import annotations
import json, pathlib, socket, subprocess, time, sys

THIS = pathlib.Path(__file__).parent
sys.path.insert(0, str(THIS))
from demo_sync import step_both_to_gm07_and_sync  # noqa: E402

REPO = THIS.parent.parent.parent
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


def diff_addrs(rec, emu):
    """Return set of meaningful diff offsets (skip DP scratch + stack)."""
    s = set()
    for i in range(min(len(rec), len(emu))):
        if rec[i] != emu[i]:
            if i < 0x20 or 0x100 <= i < 0x200:
                continue
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

        info = step_both_to_gm07_and_sync(sock, f, verbose=True)
        if not info['synced']:
            print('FATAL: demo-phase sync failed'); return

        # Snapshot at sync.
        rec0 = _read_range(sock, f, 'rec', 0x0000, 0x8000)
        emu0 = _read_range(sock, f, 'emu', 0x0000, 0x8000)
        baseline = diff_addrs(rec0, emu0)
        print(f'[+0] baseline diffs: {len(baseline)}')

        # Step lockstep, find first frame with NEW diffs (beyond baseline).
        for fi in range(1, 31):
            cmd(sock, f, 'step 1'); cmd(sock, f, 'emu_step 1')
            rec = _read_range(sock, f, 'rec', 0x0000, 0x8000)
            emu = _read_range(sock, f, 'emu', 0x0000, 0x8000)
            d = diff_addrs(rec, emu)
            new = d - baseline
            healed = baseline - d
            if new:
                print(f'\n[+{fi}] NEW diffs: {len(new)}, healed: {len(healed)}')
                for a in sorted(new)[:30]:
                    print(f'    ${a:04x}: rec=${rec[a]:02x} emu=${emu[a]:02x}'
                          f' (was rec=${rec0[a]:02x} emu=${emu0[a]:02x})')
                if len(new) > 30:
                    print(f'    +{len(new) - 30} more')
                break
            else:
                print(f'[+{fi}] no new diffs (total {len(d)}, healed {len(healed)})')
        else:
            print(f'\n[+30] still no NEW diffs vs baseline; everything matches except the 10 entry residue')
    finally:
        try: sock.close()
        except Exception: pass
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill()
        _kill()


if __name__ == '__main__':
    main()
