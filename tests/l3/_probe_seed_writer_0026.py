"""Trace writers to $0026 on both sides at lockstep GM=07 sync.
At sync: rec=$00 emu=$82 — somebody wrote $0026 on emu but not
on rec. The first divergent write identifies the divergent code
path."""
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

        # Step rec to GM=07 (auto-locksteps emu).
        rs = 0
        for _ in range(3000):
            cmd(sock, f, 'step 1'); rs += 1
            if int(cmd(sock, f, 'dump_ram 0x100 1')['hex'].replace(' ',''),16) == 0x07:
                break
        print(f'[at GM=07 sync] rec_steps={rs}')

        # Query always-on traces for writers to $0026.
        for addr in (0x26, 0x27, 0x45, 0x46, 0x47, 0x70, 0x7b, 0x7d):
            rt = cmd(sock, f, f'wram_writes_at {addr:x} 0 999999 16')
            et = cmd(sock, f, f'emu_wram_writes_at {addr:x} 0 999999 16')
            rec_writes = rt.get('matches', [])
            emu_writes = et.get('matches', [])
            print(f'\n=== ${addr:02x} ===')
            print(f'  rec: {len(rec_writes)} writes')
            for e in rec_writes[:8]:
                print(f'    f={e["f"]:3} val={e["val"]:>6} func={e["func"][:32]:32} parent={e["parent"][:32]}')
            print(f'  emu: {len(emu_writes)} writes')
            for e in emu_writes[:8]:
                print(f'    f={e["f"]:5} pc={e["pc"]} {e["before"]}->{e["after"]}')
    finally:
        try: sock.close()
        except Exception: pass
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill()
        _kill()


if __name__ == '__main__':
    main()
