"""Trace writes to OAM slot 68's TileNo byte ($0311) during Mario's
jump frame. Diff recomp vs oracle to find which function emits the
wrong tile index."""
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

    # Arm traces on OAM slot 68 tile + slot 69 tile (Mario's body tiles)
    cmd(s, f, 'trace_wram_reset'); cmd(s, f, 'emu_wram_trace_reset')
    for addr in (0x0311, 0x0315):
        cmd(s, f, f'trace_wram {addr:x} {addr:x}')
        cmd(s, f, f'emu_wram_trace_add {addr:x} {addr:x}')

    # Mode 0x07 both sides
    for _ in range(2000):
        cmd(s, f, 'step 1')
        if int(cmd(s, f, 'dump_ram 0x100 1')['hex'].replace(' ', ''), 16) == 7: break
    for _ in range(2000):
        cmd(s, f, 'emu_step 1')
        if int(cmd(s, f, 'emu_read_wram 0x100 1')['hex'].replace(' ', ''), 16) == 7: break

    # Step until both are airborne (Mario jumping)
    for _ in range(50):
        cmd(s, f, 'step 1'); cmd(s, f, 'emu_step 1')
        r_air = int(cmd(s, f, 'dump_ram 0x72 1')['hex'].replace(' ', ''), 16)
        o_air = int(cmd(s, f, 'emu_read_wram 0x72 1')['hex'].replace(' ', ''), 16)
        if r_air and o_air: break

    # Step a couple more frames to catch jump-pose rendering
    for _ in range(3):
        cmd(s, f, 'step 1'); cmd(s, f, 'emu_step 1')

    print('=== recomp writes to $0311 / $0315 ===')
    r = cmd(s, f, 'get_wram_trace')
    for e in r.get('log', [])[-40:]:
        adr = int(e.get('adr', '0x0'), 16)
        if adr not in (0x0311, 0x0315): continue
        print(f'  f{e["f"]:4} {e["adr"]} {e["old"]}->{e["val"]} '
              f'func={e["func"]:60s} parent={e["parent"]}')

    print('\n=== oracle writes to $0311 / $0315 ===')
    r = cmd(s, f, 'emu_get_wram_trace')
    for e in r.get('log', [])[-40:]:
        adr = int(e.get('adr', '0x0'), 16)
        if adr not in (0x0311, 0x0315): continue
        pc = e.get('pc', '?')
        print(f'  f{e["f"]:4} {e["adr"]} {e.get("before","?")}->{e["after"]} '
              f'pc={pc} bank={e.get("bank_src","?")}')
finally:
    s.close()
    p.terminate()
    try: p.wait(timeout=5)
    except subprocess.TimeoutExpired: p.kill()
    subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
