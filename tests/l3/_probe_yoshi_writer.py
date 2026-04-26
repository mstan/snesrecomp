"""Issue C — find which recomp function writes the wrong Yoshi
Y velocity ($F0 / -16 signed) to slot 0's Y-vel byte ($00AA) on
spawn from the ?-block in attract demo.

Workflow:
  1. Launch oracle build paused.
  2. Arm trace_wram on a tight range covering slot 0's Y-velocity
     ($00AA, 1 byte). Optionally also slot 0's X-velocity ($00B6)
     and slot 0's sprite type ($009E) so we see when Yoshi was
     installed in the slot.
  3. Step recomp through the attract demo until past the Yoshi-
     spawn frame (~690).
  4. Pull the WRAM-write trace and dump every (frame, addr, value,
     writer_function) record.
  5. Filter to writes that put $F0 into $00AA — those rows name
     the recomp function that produces the bug.

Output: a table of recent $00AA writes and their writer functions.
"""
from __future__ import annotations
import json
import pathlib
import socket
import subprocess
import sys
import time

REPO = pathlib.Path(__file__).absolute().parents[3]
ORACLE_EXE = REPO / 'build' / 'bin-x64-Oracle' / 'smw.exe'
PORT = 4377


def kill_existing():
    subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'],
                   capture_output=True, check=False)


def launch():
    if not ORACLE_EXE.exists():
        sys.exit(f'oracle build missing: {ORACLE_EXE}')
    kill_existing()
    time.sleep(0.5)
    p = subprocess.Popen(
        [str(ORACLE_EXE), '--paused'],
        cwd=str(REPO),
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    deadline = time.time() + 15
    while time.time() < deadline:
        try:
            s = socket.create_connection(('127.0.0.1', PORT), timeout=0.3)
            s.close()
            time.sleep(0.3)
            return p
        except OSError:
            time.sleep(0.2)
    p.kill()
    sys.exit('timeout')


class Client:
    def __init__(self, port):
        self.sock = socket.create_connection(
            ('127.0.0.1', port), timeout=600)
        self.f = self.sock.makefile('rwb')
        self.f.readline()
    def cmd(self, line):
        self.sock.sendall((line + '\n').encode())
        return json.loads(self.f.readline())
    def close(self):
        try: self.sock.close()
        except OSError: pass


def step_to(c, target):
    cur = c.cmd('frame').get('frame', 0)
    if cur >= target: return cur
    c.cmd(f'step {target - cur}')
    deadline = time.time() + 60
    while time.time() < deadline:
        cur = c.cmd('frame').get('frame', cur)
        if cur >= target: break
        time.sleep(0.1)
    return cur


def main():
    proc = launch()
    try:
        c = Client(PORT)
        try:
            print('Issue C — track $00AA (slot 0 Y-vel) writers')
            print()

            # Arm trace BEFORE the spawn moment. Range covers slot 0's
            # Y-vel ($AA), X-vel ($B6), and sprite type ($9E) — three
            # 1-byte addresses, so use three independent traces (the
            # range form is `trace_wram <lo> [hi]`).
            c.cmd('trace_wram_reset')
            c.cmd('trace_wram aa aa')
            c.cmd('trace_wram b6 b6')
            c.cmd('trace_wram 9e 9e')
            print('  armed trace_wram on $9E, $AA, $B6 (slot 0 type/y-vel/x-vel)')

            # Step to frame 720 (past spawn).
            print('  stepping to frame 720...')
            step_to(c, 720)

            # Pull the trace.
            r = c.cmd('get_wram_trace')
            log = r.get('log', [])
            print(f'  trace: {len(log)} writes captured')
            print()
            if not log:
                print('  No writes in trace — trace_wram syntax may differ.')
                print(f'  Raw response: {r}')
                return

            # Print the writes in chronological order, focused on the
            # spawn frame range.
            # JSON keys per cmd_get_wram_trace: f, adr, old, val, func, parent.
            print(f'  {"frame":>5}  {"addr":>7}  '
                  f'{"old":>6} -> {"val":>6}  writer  (parent)')
            print(f'  {"-"*5}  {"-"*7}  {"-"*6}    {"-"*6}  {"-"*40}')
            for e in log:
                f = e.get('f', '?')
                addr = e.get('adr', '?')
                old = e.get('old', '?')
                val = e.get('val', '?')
                fn = e.get('func', '?')
                pa = e.get('parent', '')
                marker = ''
                if isinstance(addr, str) and addr.endswith('00aa') \
                   and isinstance(val, str) and val.lower().endswith('f0'):
                    marker = '  <- $00AA <- $F0 (Yoshi Y-vel bug)'
                print(f'  {f:>5}  {addr:>7}  {old:>6} -> {val:>6}  '
                      f'{fn} ({pa}){marker}')
        finally:
            c.close()
    finally:
        proc.kill()
        kill_existing()


if __name__ == '__main__':
    main()
