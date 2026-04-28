"""State-sync diff using the CORRECT lockstep model.

main.c:785-787 calls emu_oracle_run_frame inside every
RtlRunFrame. So `step N` automatically advances both recomp
and snes9x by N frames. `emu_step` is for ADDITIONAL emu
advancement (rarely useful), not for lockstep.

Prior probes that did `step 1; emu_step 1` were DOUBLE-STEPPING
emu. The OAM residue diffs at GM=07 entry may have been a
probe artifact from this over-stepping.

This probe uses ONLY `step` and queries emu state at the end.
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

        # Step ONLY recomp until rec sees GM=07. Emu advances in
        # lockstep automatically via emu_oracle_run_frame.
        rec_steps = 0
        for _ in range(3000):
            cmd(sock, f, 'step 1'); rec_steps += 1
            gm = cmd(sock, f, 'dump_ram 0x100 1')['hex'].replace(' ', '')
            if int(gm, 16) == 0x07: break
        ef = cmd(sock, f, 'emu_frame').get('frame', '?')
        print(f'[rec at GM=07 in {rec_steps} steps; emu_frame={ef}]')

        # Check: does emu also see GM=07 at this same logical moment?
        emu_gm = cmd(sock, f, 'emu_read_wram 0x100 1')['hex'].replace(' ', '')
        print(f'  emu $0100 = ${int(emu_gm, 16):02x}')

        # State-sync diff at this lockstep moment.
        rec = _read_range(sock, f, 'rec', 0x0000, 0x8000)
        emu = _read_range(sock, f, 'emu', 0x0000, 0x8000)
        diffs = []
        for i in range(0x8000):
            if rec[i] != emu[i]:
                diffs.append((i, rec[i], emu[i]))
        meaningful = [(a, r, e) for a, r, e in diffs
                      if not (a < 0x20 or 0x100 <= a < 0x200)]
        print(f'\n[diff at lockstep GM=07-rec moment]')
        print(f'  total raw: {len(diffs)}')
        print(f'  meaningful: {len(meaningful)}')
        print(f'  baseline (over-stepped probe): 10')
        for a, r, e in meaningful[:30]:
            print(f'    ${a:04x}: rec=${r:02x} emu=${e:02x}')
        if len(meaningful) > 30:
            print(f'    +{len(meaningful) - 30} more')

        # If emu hasn't hit GM=07 yet, step both more until it does.
        if int(emu_gm, 16) != 0x07:
            print(f'\n  emu not yet at GM=07; stepping until both are sync')
            extra = 0
            for _ in range(500):
                cmd(sock, f, 'step 1'); extra += 1
                emu_gm = cmd(sock, f, 'emu_read_wram 0x100 1')['hex'].replace(' ', '')
                if int(emu_gm, 16) == 0x07: break
            print(f'  +{extra} extra steps; emu_$0100=${int(emu_gm, 16):02x}')
            ef = cmd(sock, f, 'emu_frame').get('frame', '?')
            print(f'  emu_frame now = {ef}, rec_steps now = {rec_steps + extra}')
            rec = _read_range(sock, f, 'rec', 0x0000, 0x8000)
            emu = _read_range(sock, f, 'emu', 0x0000, 0x8000)
            diffs = []
            for i in range(0x8000):
                if rec[i] != emu[i]:
                    diffs.append((i, rec[i], emu[i]))
            meaningful = [(a, r, e) for a, r, e in diffs
                          if not (a < 0x20 or 0x100 <= a < 0x200)]
            print(f'  diff after both at GM=07: {len(meaningful)} meaningful')
            for a, r, e in meaningful[:20]:
                print(f'    ${a:04x}: rec=${r:02x} emu=${e:02x}')
    finally:
        try: sock.close()
        except Exception: pass
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill()
        _kill()


if __name__ == '__main__':
    main()
