"""Bug #8 — trace $0100 (GameMode) writes on both sides. On recomp
mode 0x03 lasts 1 frame; on oracle it lasts 20. Find every function
that writes $0100 on each side, ordered by frame; compare the
sequence to identify which mode-advance is premature."""
from __future__ import annotations
import json, pathlib, socket, subprocess, sys, time

REPO = pathlib.Path(r'F:/Projects/SuperMarioWorldRecomp')
EXE = REPO / 'build/bin-x64-Oracle/smw.exe'
MAX_FRAME_RECOMP = 400


def cmd(sock, f, line):
    sock.sendall((line + '\n').encode())
    return json.loads(f.readline())


def step1(sock, f):
    before = cmd(sock, f, 'frame').get('frame', 0)
    cmd(sock, f, 'step 1')
    deadline = time.time() + 5
    while time.time() < deadline:
        if cmd(sock, f, 'frame').get('frame', 0) > before:
            return before + 1
        time.sleep(0.01)


def main():
    subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.5)
    proc = subprocess.Popen([str(EXE), '--paused'],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, cwd=str(REPO))
    try:
        sock = socket.socket()
        for _ in range(50):
            try: sock.connect(('127.0.0.1', 4377)); break
            except (ConnectionRefusedError, OSError): time.sleep(0.2)
        f = sock.makefile('r')
        f.readline()

        cmd(sock, f, 'trace_wram_reset')
        cmd(sock, f, 'trace_wram 100 100')
        cmd(sock, f, 'emu_wram_trace_reset')
        cmd(sock, f, 'emu_wram_trace_add 100 100')

        for fr in range(MAX_FRAME_RECOMP):
            step1(sock, f)

        r = cmd(sock, f, 'get_wram_trace')
        rlog = r.get('log', [])
        print(f'recomp $0100 writes ({len(rlog)}):')
        for e in rlog:
            print(f'  f{e.get("f"):4} {e.get("old","?")}->{e.get("val")}  '
                  f'func={e.get("func")} parent={e.get("parent")}')

        r = cmd(sock, f, 'emu_get_wram_trace')
        elog = r.get('log', [])
        print(f'\noracle $0100 writes ({len(elog)}):')
        for e in elog:
            print(f'  f{e.get("f"):4} {e.get("before")}->{e.get("after")}  '
                  f'pc={e.get("pc")} bank={e.get("bank_src")}')
        return 0
    finally:
        try: sock.close()
        except Exception: pass
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill()
        subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


if __name__ == '__main__':
    sys.exit(main())
