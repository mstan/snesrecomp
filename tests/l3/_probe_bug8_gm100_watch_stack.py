"""Bug #8 — use Tier 2.5 watchpoint on $0100 (match val 0x04) to pause
recomp at the instant GameMode goes to 0x04 on frame 94, then dump the
full recomp call stack via g_recomp_stack to find the actual caller
chain that triggered the premature mode advance.

Tooling step required: `parked` currently only shows `writer` (single
function name). This probe uses watch_add + step loop, and when parked,
issues `get_call_trace contains=LoadSublevel` to cross-reference the
call history and identify the actual stack around the write."""
from __future__ import annotations
import json, pathlib, socket, subprocess, sys, time

REPO = pathlib.Path(r'F:/Projects/SuperMarioWorldRecomp')
EXE = REPO / 'build/bin-x64-Oracle/smw.exe'
MAX_FRAMES = 250


def cmd(sock, f, line):
    sock.sendall((line + '\n').encode())
    return json.loads(f.readline())


def step_unsafe(sock, f, n):
    # Issue step without polling - we want to trip the watch.
    cmd(sock, f, f'step {n}')


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

        # Arm watchpoint on $0100 = 0x04 (the premature transition).
        r = cmd(sock, f, 'watch_add 100 04')
        print(f'watch armed: {r}')
        # Also arm trace_calls to get a call history.
        cmd(sock, f, 'trace_calls_reset')
        cmd(sock, f, 'trace_calls')

        # Advance frames; the watchpoint parks on hit.
        cmd(sock, f, f'step {MAX_FRAMES}')
        time.sleep(2.0)  # let the game run until it parks or finishes

        # Poll parked state.
        for _ in range(40):
            r = cmd(sock, f, 'parked')
            if r.get('parked'):
                print(f'\nPARKED: {r}')
                break
            time.sleep(0.1)
        else:
            print('did not park within 4s — watchpoint did not trip')
            return 2

        # Find the trace_calls entries around the parked frame.
        frame_now = cmd(sock, f, 'frame').get('frame', 0)
        print(f'\ncurrent frame: {frame_now}')

        # Grab ALL call entries (no filter) for frames >= frame_now - 2.
        # get_call_trace accepts from/to filters.
        r = cmd(sock, f, f'get_call_trace from={max(0, frame_now-3)} to={frame_now}')
        entries = r.get('log', [])
        print(f'\ncall trace entries around park: {len(entries)} (may be truncated)')
        # Show only the last 60 entries to see the chain leading to LoadSublevel.
        for e in entries[-60:]:
            print(f'  f{e.get("f"):4} d{e.get("d"):2} {e.get("func"):<48}  <- {e.get("parent")}')

        # Allow continue so the process cleans up.
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
