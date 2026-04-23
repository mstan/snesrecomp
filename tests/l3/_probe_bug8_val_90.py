"""Watch $90 (PlayerYPosInBlock) on recomp and oracle around the
critical moment. EB77's $EE1B branch tests $90 & 0x0F >= 8; if < 8,
calls EE3A (clears $72); if >= 8, falls through to EE1D (sets $72=0x24).

Oracle takes EE3A branch (clears $72) at oracle f296.
Recomp takes EE1D branch. So their $90 values at that instant differ."""
from __future__ import annotations
import json, pathlib, socket, subprocess, sys, time

REPO = pathlib.Path(r'F:/Projects/SuperMarioWorldRecomp')
EXE = REPO / 'build/bin-x64-Oracle/smw.exe'


def cmd(sock, f, l):
    sock.sendall((l + '\n').encode())
    return json.loads(f.readline())


def step1(sock, f):
    b = cmd(sock, f, 'frame').get('frame', 0)
    cmd(sock, f, 'step 1')
    dl = time.time() + 5
    while time.time() < dl:
        if cmd(sock, f, 'frame').get('frame', 0) > b: return b + 1
        time.sleep(0.01)


def main():
    subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.5)
    p = subprocess.Popen([str(EXE), '--paused'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, cwd=str(REPO))
    try:
        sock = socket.socket()
        for _ in range(50):
            try: sock.connect(('127.0.0.1', 4377)); break
            except (ConnectionRefusedError, OSError): time.sleep(0.2)
        f = sock.makefile('r'); f.readline()

        cmd(sock, f, 'trace_wram_reset'); cmd(sock, f, 'trace_wram 90 91')
        cmd(sock, f, 'emu_wram_trace_reset'); cmd(sock, f, 'emu_wram_trace_add 90 91')
        # Advance both.
        for _ in range(150):
            step1(sock, f)
        # Catch oracle up.
        while int(cmd(sock, f, 'emu_read_wram 0x100 1')['hex'].replace(' ',''), 16) != 0x07:
            cmd(sock, f, 'emu_step 1')

        r = cmd(sock, f, 'get_wram_trace')
        rlog = r.get('log', [])
        print(f'=== recomp $90/$91 writes ({len(rlog)}) ===')
        for e in rlog[:30]:
            print(f'  f{e.get("f"):4} {e.get("adr")} {e.get("old","?")}->{e.get("val")} '
                  f'func={e.get("func")}')

        r = cmd(sock, f, 'emu_get_wram_trace')
        elog = r.get('log', [])
        print(f'\n=== oracle $90/$91 writes ({len(elog)}) ===')
        for e in elog[:30]:
            print(f'  f{e.get("f"):4} {e.get("adr")} {e.get("before")}->{e.get("after")} '
                  f'pc={e.get("pc")}')
        return 0
    finally:
        try: sock.close()
        except Exception: pass
        p.terminate()
        try: p.wait(timeout=5)
        except subprocess.TimeoutExpired: p.kill()
        subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


if __name__ == '__main__':
    sys.exit(main())
