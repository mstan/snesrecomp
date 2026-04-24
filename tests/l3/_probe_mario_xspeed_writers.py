"""Trace who writes Mario's X-speed ($007B) on both sides on the
first physics frame after mode 0x07 entry."""
from __future__ import annotations
import json, pathlib, socket, subprocess, sys, time
REPO = pathlib.Path(r'F:/Projects/SuperMarioWorldRecomp')
EXE = REPO / 'build/bin-x64-Oracle/smw.exe'

def cmd(s, f, line):
    s.sendall((line + '\n').encode()); return json.loads(f.readline())

subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'],
               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(0.5)
p = subprocess.Popen([str(EXE), '--paused'], cwd=REPO,
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    s = socket.socket()
    for _ in range(50):
        try: s.connect(('127.0.0.1', 4377)); break
        except (ConnectionRefusedError, OSError): time.sleep(0.2)
    f = s.makefile('r'); f.readline()

    # Arm write traces on PlayerXSpeed ($7B) for both sides + related fields
    cmd(s, f, 'trace_wram_reset')
    cmd(s, f, 'emu_wram_trace_reset')
    for addr in (0x7b, 0x7c, 0x7d):  # X speed, Y speed_lo, Y speed_hi
        cmd(s, f, f'trace_wram {addr:x} {addr:x}')
        cmd(s, f, f'emu_wram_trace_add {addr:x} {addr:x}')

    # Advance both to mode 0x07
    MODE = 0x07
    for _ in range(2000):
        cmd(s, f, 'step 1')
        if int(cmd(s, f, 'dump_ram 0x100 1')['hex'].replace(' ', ''), 16) == MODE: break
    for _ in range(2000):
        cmd(s, f, 'emu_step 1')
        if int(cmd(s, f, 'emu_read_wram 0x100 1')['hex'].replace(' ', ''), 16) == MODE: break

    # Step 5 frames each
    for _ in range(5):
        cmd(s, f, 'step 1')
        cmd(s, f, 'emu_step 1')

    # Dump recomp writes
    r = cmd(s, f, 'get_wram_trace')
    print(f'=== recomp writes to $7B/$7C/$7D (last 40) ===')
    for e in r.get('log', [])[-40:]:
        adr = int(e.get('adr', '0x0'), 16)
        if adr not in (0x7b, 0x7c, 0x7d): continue
        lbl = {0x7b: 'XSp', 0x7c: 'YSpLo', 0x7d: 'YSpHi'}[adr]
        print(f'  f{e["f"]:3} {lbl:5s} {e.get("old","?")}->{e.get("val")} '
              f'func={e.get("func")} parent={e.get("parent")}')

    r = cmd(s, f, 'emu_get_wram_trace')
    print(f'\n=== oracle writes to $7B/$7C/$7D (last 40) ===')
    for e in r.get('log', [])[-40:]:
        adr = int(e.get('adr', '0x0'), 16)
        if adr not in (0x7b, 0x7c, 0x7d): continue
        lbl = {0x7b: 'XSp', 0x7c: 'YSpLo', 0x7d: 'YSpHi'}[adr]
        pc = e.get('pc', '?')
        print(f'  f{e["f"]:3} {lbl:5s} {e.get("before","?")}->{e.get("after")} '
              f'pc={pc} bank={e.get("bank_src","?")}')
finally:
    s.close()
    p.terminate()
    try: p.wait(timeout=5)
    except subprocess.TimeoutExpired: p.kill()
    subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
