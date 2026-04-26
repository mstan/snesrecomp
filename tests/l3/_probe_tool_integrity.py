"""Tool-integrity check: before drawing conclusions about recomp's
slot-8 emptiness, verify that read_ram and emu_read_wram are
actually returning meaningful data from both sides at a deep
frame.

Tests, in order:
  1. Step both sides to frame 690 (Yoshi-spawn frame from earlier
     probe). Confirm recomp's `frame` command reports 690.
  2. Read $0100 (GameMode) from BOTH. Boot-smoke baseline shows
     this should be 0x05 by frame 100; at 690 it's still in
     mode-X. If recomp returns 0 here, the tool is broken or
     recomp is wedged.
  3. Read $14C8..$14D3 (sprite status, all 12 slots) from both.
     Each side reports its own picture of which slots are alive.
  4. Read $9E..$A9 (sprite type, all 12 slots) from both. If
     recomp returns all zeros while oracle has live types, that's
     a real divergence (sprites not spawning); if both are zero,
     attract demo's level hasn't started yet on recomp side.
  5. Read $1F00..$1F0F (post-fuzz-epilogue scratch — known to be
     written during attract by other paths). Sanity check the
     reader can return non-zero recomp WRAM.

If the reader works for $0100 / $1F00 but slot tables differ →
real recomp behavior divergence. If reader returns zeros for
addresses we know should be non-zero → tool gap, fix first.
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
    sys.exit('timeout waiting for oracle TCP port')


class Client:
    def __init__(self, port: int):
        self.sock = socket.create_connection(
            ('127.0.0.1', port), timeout=600)
        self.f = self.sock.makefile('rwb')
        self.f.readline()
    def cmd(self, line: str) -> dict:
        self.sock.sendall((line + '\n').encode())
        return json.loads(self.f.readline())
    def close(self):
        try: self.sock.close()
        except OSError: pass


def hex_to_bytes(s: str) -> bytes:
    return bytes.fromhex(s) if s else b''


def step_recomp_to(client, target):
    cur = client.cmd('frame').get('frame', 0)
    if cur >= target: return cur
    client.cmd(f'step {target - cur}')
    deadline = time.time() + 60
    while time.time() < deadline:
        cur = client.cmd('frame').get('frame', cur)
        if cur >= target: break
        time.sleep(0.1)
    return cur


def main():
    proc = launch()
    try:
        c = Client(PORT)
        try:
            print('Tool-integrity check before drawing slot-8 conclusions')
            print()

            # 1. Sync both sides to frame 690.
            print('  Step both sides to frame 690...')
            r_frame = step_recomp_to(c, 690)
            c.cmd('emu_step 690')
            print(f'    recomp `frame` reports: {r_frame}')
            print(f'    recomp `frame` (re-read after emu_step): '
                  f'{c.cmd("frame").get("frame")}')
            print()

            # 2. GameMode at $0100.
            r_gm = hex_to_bytes(c.cmd('read_ram 100 1').get('hex', ''))
            o_gm = hex_to_bytes(c.cmd('emu_read_wram 100 1').get('hex', ''))
            print(f'  $0100 GameMode    recomp={r_gm.hex()} '
                  f'oracle={o_gm.hex()}')

            # 3. Sprite status table.
            r_st = hex_to_bytes(c.cmd('read_ram 14c8 12').get('hex', ''))
            o_st = hex_to_bytes(c.cmd('emu_read_wram 14c8 12').get('hex', ''))
            print(f'  $14C8 SprStatus   recomp={r_st.hex()} '
                  f'oracle={o_st.hex()}')

            # 4. Sprite type table.
            r_ty = hex_to_bytes(c.cmd('read_ram 9e 12').get('hex', ''))
            o_ty = hex_to_bytes(c.cmd('emu_read_wram 9e 12').get('hex', ''))
            print(f'  $009E SprType     recomp={r_ty.hex()} '
                  f'oracle={o_ty.hex()}')

            # 5. Sanity sprite Y position low.
            r_y = hex_to_bytes(c.cmd('read_ram d8 12').get('hex', ''))
            o_y = hex_to_bytes(c.cmd('emu_read_wram d8 12').get('hex', ''))
            print(f'  $00D8 SprYLo      recomp={r_y.hex()} '
                  f'oracle={o_y.hex()}')

            # 6. $1F00..$1F0F — sanity check reader can return non-zero.
            r_1f = hex_to_bytes(c.cmd('read_ram 1f00 16').get('hex', ''))
            o_1f = hex_to_bytes(c.cmd('emu_read_wram 1f00 16').get('hex', ''))
            print(f'  $1F00 scratch     recomp={r_1f.hex()} '
                  f'oracle={o_1f.hex()}')

            # 7. Mario state — known-live for the entire attract demo.
            r_mario = hex_to_bytes(c.cmd('read_ram 70 16').get('hex', ''))
            o_mario = hex_to_bytes(c.cmd('emu_read_wram 70 16').get('hex', ''))
            print(f'  $0070 Mario       recomp={r_mario.hex()} '
                  f'oracle={o_mario.hex()}')

            print()
            print('Verdict matrix:')
            for label, r, o in [
                ('$0100 GameMode',    r_gm, o_gm),
                ('$14C8 SprStatus',   r_st, o_st),
                ('$009E SprType',     r_ty, o_ty),
                ('$00D8 SprYLo',      r_y,  o_y),
                ('$1F00 scratch',     r_1f, o_1f),
                ('$0070 Mario',       r_mario, o_mario),
            ]:
                if not r:
                    verdict = 'TOOL-FAIL: recomp read returned no data'
                elif r == bytes(len(r)):
                    verdict = 'recomp all-zeros'
                elif r == o:
                    verdict = 'IDENTICAL'
                else:
                    verdict = 'differs'
                print(f'  {label:24}  {verdict}')
        finally:
            c.close()
    finally:
        proc.kill()
        kill_existing()


if __name__ == '__main__':
    main()
