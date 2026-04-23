"""Bug #8 — trace every write to $0072 (PlayerInAir) on both sides
through the boot + title-load path, up to each side's mode-0x07 entry.
Compare the write streams; the LAST write on each side before mode
0x07 is what sets the contested value (recomp=0x24 vs oracle=0x00).

Both sides run lock-step under the main `step 1` (recomp frame advance
also ticks the oracle once). Oracle may still be behind after recomp
hits 0x07 (boot-timing delta ~204 frames); we drive it forward with
`emu_step 1` until it catches up.
"""
from __future__ import annotations
import json
import pathlib
import socket
import subprocess
import sys
import time


REPO = pathlib.Path(r'F:/Projects/SuperMarioWorldRecomp')
EXE = REPO / 'build/bin-x64-Oracle/smw.exe'
TARGET_MODE = 0x07
ADDR = 0x72
MAX_BOOT_FRAMES = 2000


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
    return before


def recomp_mode(sock, f):
    return int(cmd(sock, f, 'dump_ram 0x100 1')['hex'].replace(' ', ''), 16)


def oracle_mode(sock, f):
    r = cmd(sock, f, 'emu_read_wram 0x100 1')
    return int(r['hex'].replace(' ', ''), 16) if r.get('ok') else None


def main():
    if not EXE.exists():
        print(f'ERROR: Oracle exe not found at {EXE}', file=sys.stderr)
        return 1
    subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.5)
    proc = subprocess.Popen(
        [str(EXE), '--paused'],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        cwd=str(REPO),
    )

    try:
        sock = socket.socket()
        for _ in range(50):
            try: sock.connect(('127.0.0.1', 4377)); break
            except (ConnectionRefusedError, OSError): time.sleep(0.2)
        f = sock.makefile('r')
        banner = f.readline()
        if 'connected' not in banner:
            print(f'unexpected banner: {banner!r}', file=sys.stderr)
            return 1

        # Arm the two traces on $0072 BEFORE any stepping.
        r = cmd(sock, f, 'trace_wram_reset')
        r = cmd(sock, f, f'trace_wram {ADDR:x} {ADDR:x}')
        print(f'recomp trace_wram: {r}')
        r = cmd(sock, f, 'emu_wram_trace_reset')
        r = cmd(sock, f, f'emu_wram_trace_add {ADDR:x} {ADDR:x}')
        print(f'oracle emu_wram_trace_add: {r}')

        # Advance both until recomp hits mode 0x07.
        rframe = None
        for frame in range(1, MAX_BOOT_FRAMES + 1):
            step1(sock, f)
            if recomp_mode(sock, f) == TARGET_MODE:
                rframe = frame
                break
        if rframe is None:
            print('FAIL: recomp never reached 0x07')
            return 2
        print(f'recomp hit 0x07 at frame {rframe}')

        # Push oracle until it also hits 0x07.
        oracle_extra = 0
        while oracle_mode(sock, f) != TARGET_MODE and oracle_extra < MAX_BOOT_FRAMES:
            cmd(sock, f, 'emu_step 1')
            oracle_extra += 1
        print(f'oracle hit 0x07 after {oracle_extra} extra emu_step frames')

        # Dump the recomp write stream for $72.
        r = cmd(sock, f, 'get_wram_trace')
        rlog = r.get('log', [])
        print(f'\n=== recomp $72 writes ({len(rlog)} total) ===')
        print('  frame | addr     | old -> new | func <- parent')
        for e in rlog:
            # Limit to the last 30 entries for readability.
            pass
        for e in rlog[-30:]:
            old = e.get('old', '?'); new = e.get('val'); fr = e.get('f')
            func = e.get('func'); par = e.get('parent')
            print(f'  {fr:5}  | {e.get("adr")} | {old} -> {new} | {func} <- {par}')

        # Dump the oracle (snes9x) write stream for $72.
        r = cmd(sock, f, 'emu_get_wram_trace')
        elog = r.get('log', [])
        print(f'\n=== oracle $72 writes ({len(elog)} total) ===')
        print('  frame | addr     | pc        | before -> after | bank')
        for e in elog[-30:]:
            print(f'  {e.get("f"):5}  | {e.get("adr")} | {e.get("pc")} | '
                  f'{e.get("before")} -> {e.get("after")}    | {e.get("bank_src")}')

        # Pull out the last recomp write and last oracle write.
        last_r = rlog[-1] if rlog else None
        last_o = elog[-1] if elog else None
        print('\n=== last writers ===')
        if last_r:
            print(f'  recomp: frame={last_r.get("f")} '
                  f'{last_r.get("old", "?")}->{last_r.get("val")} '
                  f'func={last_r.get("func")} parent={last_r.get("parent")}')
        else:
            print('  recomp: (no writes recorded)')
        if last_o:
            print(f'  oracle: frame={last_o.get("f")} '
                  f'{last_o.get("before")}->{last_o.get("after")} '
                  f'pc={last_o.get("pc")} bank={last_o.get("bank_src")}')
        else:
            print('  oracle: (no writes recorded)')
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
