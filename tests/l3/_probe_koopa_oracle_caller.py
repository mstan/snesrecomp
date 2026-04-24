"""Koopa-falls step 2a: capture oracle's instruction trace across the
window where slot-9 koopa physics fires, and print the ~30 instructions
preceding each hit at $019A04 (SetSomeYSpeed__).

The JSR immediately before $019A04 identifies which of the 18 callers
the walking-koopa physics handler uses. That's the parent we'll then
mirror-test on recomp."""
from __future__ import annotations
import json, pathlib, socket, subprocess, sys, time

REPO = pathlib.Path(r'F:/Projects/SuperMarioWorldRecomp')
EXE = REPO / 'build/bin-x64-Oracle/smw.exe'
TARGET_MODE = 0x07
MAX_BOOT = 2000
SET_SOME_Y_SPEED = 0x019A04


def cmd(sock, f, line):
    sock.sendall((line + '\n').encode())
    return json.loads(f.readline())


def step1(sock, f):
    b = cmd(sock, f, 'frame').get('frame', 0)
    cmd(sock, f, 'step 1')
    deadline = time.time() + 5
    while time.time() < deadline:
        if cmd(sock, f, 'frame').get('frame', 0) > b: return b + 1
        time.sleep(0.01)


def e_byte(sock, f, addr):
    r = cmd(sock, f, f'emu_read_wram 0x{addr:x} 1')
    return int(r['hex'].replace(' ', ''), 16) if r.get('ok') else -1


def e_bytes(sock, f, addr, n):
    r = cmd(sock, f, f'emu_read_wram 0x{addr:x} {n}')
    if not r.get('ok'): return []
    h = r['hex'].replace(' ', '')
    return [int(h[2*i:2*i+2], 16) for i in range(n)]


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
        f = sock.makefile('r'); f.readline()

        # Advance oracle to mode 0x07 + dwell so slot 9 koopa is alive.
        # Oracle advances in lockstep with recomp's step when you use emu_step.
        print('advancing oracle to mode 0x07...')
        for i in range(MAX_BOOT):
            cmd(sock, f, 'emu_step 1')
            if e_byte(sock, f, 0x100) == TARGET_MODE:
                print(f'  mode 0x07 reached at oracle step {i+1}')
                break

        # Find any slot with status 0x08 (walking). Oracle koopa is in
        # slot 7 per sanity-check; recomp koopa is in slot 9. The caller
        # chain is the same regardless of slot.
        koopa_slot = None
        for i in range(400):
            cmd(sock, f, 'emu_step 1')
            st = e_bytes(sock, f, 0x14C8, 12)
            for s in range(12):
                if st[s] == 0x08:
                    koopa_slot = s
                    break
            if koopa_slot is not None:
                print(f'  slot{koopa_slot} status=0x08 at dwell={i+1}')
                break
        else:
            print('  NEVER SAW any slot status=0x08')
            return 1

        yp = e_byte(sock, f, 0x00D8 + koopa_slot)
        ys = e_byte(sock, f, 0x00AA + koopa_slot)
        print(f'  slot{koopa_slot} YPosLo=0x{yp:02x} YSpeed=0x{ys:02x}')

        # Arm insn trace and step 3 frames to catch the JSR into $019A04.
        cmd(sock, f, 'emu_insn_trace_reset')
        cmd(sock, f, 'emu_insn_trace_on')
        for _ in range(5):
            cmd(sock, f, 'emu_step 1')
        cmd(sock, f, 'emu_insn_trace_off')

        r = cmd(sock, f, 'emu_insn_trace_count')
        total = int(r.get('count', 0))
        print(f'\ncaptured {total} instructions across 5 oracle frames')

        # Collect ALL entries (bounded; 5 frames ~= 150k-300k insns max).
        # We need them in-order to find the predecessor of each $019A04 hit.
        all_entries = []
        from_idx = 0
        while from_idx < total:
            r = cmd(sock, f, f'emu_get_insn_trace from={from_idx} limit=4096')
            log = r.get('log', [])
            if not log: break
            for e in log:
                all_entries.append(e)
            from_idx = int(log[-1]['i']) + 1

        print(f'  pulled {len(all_entries)} entries')

        # Find every entry whose pc == 0x019A04.
        hits = [i for i, e in enumerate(all_entries)
                if int(e['pc'], 16) == SET_SOME_Y_SPEED]
        print(f'  hits at $019A04: {len(hits)}')

        for h_i, idx in enumerate(hits[:3]):
            print(f'\n=== hit {h_i+1}/{len(hits)} at index {idx}: '
                  f'20 insns before + 5 after ===')
            lo = max(0, idx - 20); hi = min(len(all_entries), idx + 5)
            for i in range(lo, hi):
                e = all_entries[i]
                mark = '  -> ' if i == idx else '     '
                print(f'{mark}i={e["i"]:7} f{e["f"]} pc={e["pc"]} op={e["op"]} '
                      f'a={e.get("a","?")} x={e.get("x","?")} y={e.get("y","?")}')

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
