"""Bug #8 — watch $0100 = 0x04, park on hit, then look at the latest
block_trace entries leading up to the park. The last few PCs give the
exact 65816-PC context of the write, which pins the function even
when g_last_recomp_func is stale.

Also dumps the g_recomp_stack via a new get_stack command — if not
present, reports which tooling we need to add."""
from __future__ import annotations
import json, pathlib, socket, subprocess, sys, time

REPO = pathlib.Path(r'F:/Projects/SuperMarioWorldRecomp')
EXE = REPO / 'build/bin-x64-Oracle/smw.exe'


def cmd(sock, f, line):
    sock.sendall((line + '\n').encode())
    return json.loads(f.readline())


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

        cmd(sock, f, 'trace_blocks_reset')
        cmd(sock, f, 'trace_blocks')
        r = cmd(sock, f, 'watch_add 100 04')
        print(f'watch armed: {r}')

        cmd(sock, f, 'step 200')
        time.sleep(2.5)

        r = cmd(sock, f, 'parked')
        print(f'parked: {r}')

        frame = cmd(sock, f, 'frame').get('frame', 0)
        print(f'frame at park: {frame}')

        # Last 40 block entries.
        r = cmd(sock, f, f'get_block_trace from={max(0,frame-1)} to={frame}')
        log = r.get('log', [])
        print(f'\nblock trace (last 40 of {len(log)}):')
        for e in log[-40:]:
            print(f'  f{e.get("f"):4} d{e.get("d"):2} pc={e.get("pc")} func={e.get("func")}')

        cmd(sock, f, 'watch_continue')
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
