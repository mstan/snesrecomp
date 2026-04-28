"""State-sync diff at phase-pinned moment (TitleInputIndex +
VariousPromptTimer match on both sides). Compare against the
prior 10-byte GM=07-only baseline to validate Option D
(demo-phase sync as the correct lockstep methodology).

If phase-pinned diff is materially SMALLER than GM=07-only diff,
the residual divergence is genuine codegen / runtime; if it's
the SAME, the divergence isn't demo-phase-related and lives in a
different subsystem.
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
            print('FATAL: demo-phase sync failed; aborting'); return

        rec = _read_range(sock, f, 'rec', 0x0000, 0x8000)
        emu = _read_range(sock, f, 'emu', 0x0000, 0x8000)

        diffs = []
        for i in range(0x8000):
            if rec[i] != emu[i]:
                diffs.append((i, rec[i], emu[i]))
        meaningful = [(a, r, e) for a, r, e in diffs
                      if not (a < 0x20 or 0x100 <= a < 0x200)]

        print(f'\n[diff at phase-pinned sync]')
        print(f'  total raw diffs: {len(diffs)}')
        print(f'  meaningful (excl. DP scratch + stack): {len(meaningful)}')
        print(f'  baseline (GM=07-only, no phase sync): 10')
        print(f'  delta vs baseline: {len(meaningful) - 10:+d}')
        print()
        for a, r, e in meaningful[:50]:
            print(f'    ${a:04x}: rec=${r:02x} emu=${e:02x}')
        if len(meaningful) > 50:
            print(f'    ... +{len(meaningful) - 50} more')

        # Sanity-check the sync point itself.
        ti_r = rec[0x1DF4]; pt_r = rec[0x1DF5]
        ti_e = emu[0x1DF4]; pt_e = emu[0x1DF5]
        print(f'\n[sanity] TitleInputIndex/VariousPromptTimer:')
        print(f'    rec ($1DF4=${ti_r:02x}, $1DF5=${pt_r:02x})')
        print(f'    emu ($1DF4=${ti_e:02x}, $1DF5=${pt_e:02x})')
        print(f'    match: {(ti_r, pt_r) == (ti_e, pt_e)}')
    finally:
        try: sock.close()
        except Exception: pass
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill()
        _kill()


if __name__ == '__main__':
    main()
