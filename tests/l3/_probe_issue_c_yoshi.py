"""Issue C investigation: Yoshi floats up after emerging from a ?-block.

Workflow (golden-oracle, single-process Oracle build):

  1. Launch the local Oracle build (build/bin-x64-Oracle/smw.exe) so
     snes9x runs alongside recomp in one process.
  2. Step BOTH sides to a series of bracket frames. Recomp advances
     via `step N`; snes9x advances independently via `emu_step N`.
  3. At each bracket frame, read full WRAM from both sides and find
     the first byte that differs.
  4. Report the bracket where divergence first appears, plus a small
     window of context bytes for triage.

The first divergence byte tells us:
  - If divergent address is in sprite area ($14C8..$15FE), suspect
    sprite slot mismanagement.
  - If divergent address is in $7E:13D9 / $1422 / etc. (game-mode /
    level-mode), suspect mode-progression.
  - If a freshly-spawned sprite (Yoshi at sprite type $35) has wrong
    Y-velocity at sprite slot, that's exactly the visible bug.

This is a probe, not a committed test. Once we have the first
divergence, we trace its writer (call_trace / wram_trace) and
move to the framework-fix step.
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
        self.f.readline()  # banner

    def cmd(self, line: str) -> dict:
        self.sock.sendall((line + '\n').encode())
        return json.loads(self.f.readline())

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def step_recomp_to(client: Client, target_frame: int) -> int:
    cur = client.cmd('frame').get('frame', 0)
    if cur >= target_frame:
        return cur
    client.cmd(f'step {target_frame - cur}')
    deadline = time.time() + 60
    while time.time() < deadline:
        cur = client.cmd('frame').get('frame', cur)
        if cur >= target_frame:
            break
        time.sleep(0.1)
    return cur


def hex_to_bytes(s: str) -> bytes:
    if not s:
        return b''
    return bytes.fromhex(s)


def read_recomp_wram(client: Client, addr: int, n: int) -> bytes:
    r = client.cmd(f'read_ram {addr:x} {n}')
    return hex_to_bytes(r.get('hex', ''))


def read_oracle_wram(client: Client, addr: int, n: int) -> bytes:
    # emu_read_wram returns hex string in 'data' or 'hex'.
    r = client.cmd(f'emu_read_wram {addr:x} {n}')
    return hex_to_bytes(r.get('hex', '') or r.get('data', ''))


def first_divergence(a: bytes, b: bytes, base_addr: int = 0):
    n = min(len(a), len(b))
    for i in range(n):
        if a[i] != b[i]:
            return base_addr + i, a[i], b[i]
    if len(a) != len(b):
        return base_addr + n, None, None
    return None


def find_yoshi_spawn(client: Client, max_frames: int = 1200) -> int | None:
    """Step the oracle in 30-frame batches and watch for Yoshi (sprite
    type $35) appearing in any of the 12 SMW sprite slots ($009E base).
    Returns the first frame Yoshi appears, or None if not seen.
    """
    BATCH = 30
    cur = 0
    YOSHI_TYPE = 0x35
    while cur < max_frames:
        client.cmd(f'emu_step {BATCH}')
        cur += BATCH
        types = read_oracle_wram(client, 0x9E, 12)
        if any(t == YOSHI_TYPE for t in types):
            slot = next(i for i, t in enumerate(types) if t == YOSHI_TYPE)
            print(f'  Yoshi found in slot {slot} at frame ~{cur}')
            return cur
    return None


def slot_state(client, slot: int, source: str) -> dict:
    """Snapshot sprite-slot fields for one slot. SMW sprite tables:
       $9E    sprite type (1B per slot)
       $14C8  sprite status (1B per slot)
       $D8    sprite Y pos lo (1B)
       $14D4  sprite Y pos hi (1B)
       $E4    sprite X pos lo (1B)
       $14E0  sprite X pos hi (1B)
       $AA    sprite Y velocity, signed (1B)
       $B6    sprite X velocity, signed (1B)
    """
    reader = read_recomp_wram if source == 'recomp' else read_oracle_wram
    addrs = [
        ('type',     0x9E + slot,   1),
        ('status',   0x14C8 + slot, 1),
        ('y_lo',     0xD8 + slot,   1),
        ('y_hi',     0x14D4 + slot, 1),
        ('x_lo',     0xE4 + slot,   1),
        ('x_hi',     0x14E0 + slot, 1),
        ('y_vel',    0xAA + slot,   1),
        ('x_vel',    0xB6 + slot,   1),
    ]
    out = {}
    for name, addr, n in addrs:
        b = reader(client, addr, n)
        out[name] = b[0] if b else -1
    return out


def fmt_state(s: dict) -> str:
    return (f'type=${s["type"]:02X} status=${s["status"]:02X} '
            f'Y=${s["y_hi"]:02X}{s["y_lo"]:02X} '
            f'X=${s["x_hi"]:02X}{s["x_lo"]:02X} '
            f'YVel=${s["y_vel"]:02X} XVel=${s["x_vel"]:02X}')


def main():
    proc = launch()
    try:
        client = Client(PORT)
        try:
            print('Issue C probe — Yoshi spawn divergence narrowing')
            print()
            yoshi_frame = find_yoshi_spawn(client)
            if yoshi_frame is None:
                print('  Yoshi did NOT spawn in 1200 oracle frames.')
                return
            print()

            # Step recomp to the same frame.
            print(f'  syncing recomp to frame {yoshi_frame}...')
            step_recomp_to(client, yoshi_frame)

            # Find which slot RECOMP put Yoshi in (may differ from
            # oracle's slot 8). Tool-integrity probe confirmed both
            # sides spawn Yoshi but at different slot indices.
            recomp_types = list(read_recomp_wram(client, 0x9E, 12))
            oracle_types = list(read_oracle_wram(client, 0x9E, 12))
            print(f'  RECOMP types @ frame {yoshi_frame}: '
                  f'{[hex(t) for t in recomp_types]}')
            print(f'  ORACLE types @ frame {yoshi_frame}: '
                  f'{[hex(t) for t in oracle_types]}')
            r_slots = [i for i, t in enumerate(recomp_types) if t == 0x35]
            o_slots = [i for i, t in enumerate(oracle_types) if t == 0x35]
            print(f'  RECOMP Yoshi slot(s): {r_slots}   '
                  f'ORACLE Yoshi slot(s): {o_slots}')
            if not r_slots:
                print('  Recomp has NO Yoshi at this frame — different '
                      'attract progression. Cannot compare per-slot.')
                return
            r_slot = r_slots[0]
            o_slot = o_slots[0] if o_slots else 8
            print()
            print(f'  Comparing recomp slot {r_slot} vs oracle slot '
                  f'{o_slot} (Yoshi on each side):')
            print()
            cur = yoshi_frame
            print(f'  frame  side    {"sprite state":52}')
            print(f'  -----  ------  {"-"*52}')
            for tf in (yoshi_frame, yoshi_frame + 15,
                       yoshi_frame + 30, yoshi_frame + 60,
                       yoshi_frame + 120):
                if tf > cur:
                    delta = tf - cur
                    step_recomp_to(client, tf)
                    client.cmd(f'emu_step {delta}')
                    cur = tf
                r = slot_state(client, r_slot, 'recomp')
                o = slot_state(client, o_slot, 'oracle')
                print(f'  {cur:5d}  recomp  {fmt_state(r)}')
                print(f'         oracle  {fmt_state(o)}')
                if r != o:
                    diffs = [k for k in r if r[k] != o[k]]
                    print(f'         DIFF: {", ".join(diffs)}')
                print()
            return
            # Bracket frames covering attract-demo level run. Yoshi
            # spawns deeper into the demo; sweep then narrow.
            brackets = [100, 200, 300, 400, 500, 600]
            prev_tf = 0
            first_div_frame = None
            first_div_addr = None
            # Skip volatile direct-page scratch ($0000-$00FF) and the
            # stack page ($0100-$01FF). These flicker every insn and
            # never reflect meaningful state divergence — including
            # them just buries the real signal.
            DIFF_START = 0x0200
            for tf in brackets:
                # Step both sides to target — RELATIVE delta from
                # last position so neither side over-advances.
                delta = tf - prev_tf
                actual = step_recomp_to(client, tf)
                client.cmd(f'emu_step {delta}')
                prev_tf = tf
                # Read WRAM from both, $0200 → $1FFFF, in 4 KB chunks.
                CHUNK = 0x1000
                divergence = None
                for off in range(DIFF_START, 0x20000, CHUNK):
                    n = min(CHUNK, 0x20000 - off)
                    a = read_recomp_wram(client, off, n)
                    b = read_oracle_wram(client, off, n)
                    if not a or not b:
                        continue
                    d = first_divergence(a, b, base_addr=off)
                    if d:
                        divergence = d
                        break
                if divergence:
                    addr, va, vb = divergence
                    print(f'  frame {actual}: first divergence @ '
                          f'$7E:{addr:04X} '
                          f'recomp=${va:02X} oracle=${vb:02X}')
                    if first_div_frame is None:
                        first_div_frame = actual
                        first_div_addr = addr
                else:
                    print(f'  frame {actual}: WRAM byte-identical '
                          f'($0200-$1FFFF)')
            print()
            if first_div_frame is None:
                print('  No divergence in 600 frames — try larger '
                      'bracket or per-frame search.')
            else:
                print(f'  First divergence: frame {first_div_frame}, '
                      f'addr $7E:{first_div_addr:04X}.')
                print(f'  Next: narrow per-frame around frame '
                      f'{first_div_frame} and inspect addr context.')
        finally:
            client.close()
    finally:
        proc.kill()
        kill_existing()


if __name__ == '__main__':
    main()
