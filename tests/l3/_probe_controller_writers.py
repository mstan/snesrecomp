"""Who writes Controller_1 ($0016)?"""
import json, pathlib, socket, subprocess, time
REPO = pathlib.Path(r'F:/Projects/SuperMarioWorldRecomp')
EXE = REPO / 'build/bin-x64-Oracle/smw.exe'
def cmd(s, f, l):
    s.sendall((l+'\n').encode()); return json.loads(f.readline())

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
    cmd(s, f, 'trace_wram_reset'); cmd(s, f, 'emu_wram_trace_reset')
    for a in (0x15, 0x16, 0x17, 0x18):
        cmd(s, f, f'trace_wram {a:x} {a:x}')
        cmd(s, f, f'emu_wram_trace_add {a:x} {a:x}')

    for _ in range(2000):
        cmd(s, f, 'step 1')
        if int(cmd(s, f, 'dump_ram 0x100 1')['hex'].replace(' ', ''), 16) == 7: break
    for _ in range(2000):
        cmd(s, f, 'emu_step 1')
        if int(cmd(s, f, 'emu_read_wram 0x100 1')['hex'].replace(' ', ''), 16) == 7: break

    for _ in range(20): cmd(s, f, 'step 1'); cmd(s, f, 'emu_step 1')

    print('=== recomp writes to $15-$18 ===')
    for e in cmd(s, f, 'get_wram_trace').get('log', [])[-40:]:
        adr = int(e.get('adr','0x0'), 16)
        if adr not in (0x15, 0x16, 0x17, 0x18): continue
        print(f'  f{e["f"]:4} {e["adr"]} {e["old"]}->{e["val"]} '
              f'func={e["func"]:60s} parent={e["parent"]}')
    print('\n=== oracle writes to $15-$18 ===')
    for e in cmd(s, f, 'emu_get_wram_trace').get('log', [])[-40:]:
        adr = int(e.get('adr','0x0'), 16)
        if adr not in (0x15, 0x16, 0x17, 0x18): continue
        print(f'  f{e["f"]:4} {e["adr"]} {e.get("before","?")}->{e["after"]} '
              f'pc={e.get("pc","?")} bank={e.get("bank_src","?")}')
finally:
    s.close()
    p.terminate()
    try: p.wait(timeout=5)
    except subprocess.TimeoutExpired: p.kill()
    subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
