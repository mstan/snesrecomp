"""Trace writes to $0004 across the first frame of GM=0x07. Compare
recomp's writers to oracle's writers — divergent set or divergent
final value identifies the function that wrote $0004 wrongly.
"""
from __future__ import annotations
import json, pathlib, socket, subprocess, time

REPO = pathlib.Path(r'F:/Projects/SuperMarioWorldRecomp')
EXE = REPO / 'build/bin-x64-Oracle/smw.exe'


def cmd(s, f, line):
    s.sendall((line + '\n').encode())
    return json.loads(f.readline())


def main():
    subprocess.run(['taskkill','/F','/IM','smw.exe'],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.5)
    p = subprocess.Popen([str(EXE),'--paused'], cwd=REPO,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        s = socket.socket()
        for _ in range(50):
            try: s.connect(('127.0.0.1', 4377)); break
            except (ConnectionRefusedError, OSError): time.sleep(0.2)
        f = s.makefile('r'); f.readline()

        # advance both to GM=0x07
        for _ in range(2000):
            cmd(s, f, 'step 1')
            if int(cmd(s, f, 'dump_ram 0x100 1')['hex'].replace(' ',''),16) == 7: break
        for _ in range(2000):
            cmd(s, f, 'emu_step 1')
            if int(cmd(s, f, 'emu_read_wram 0x100 1')['hex'].replace(' ',''),16) == 7: break

        # Arm watchpoints on $04 (single-byte) for recomp + oracle.
        cmd(s, f, 'trace_wram_reset')
        cmd(s, f, 'emu_wram_trace_reset')
        cmd(s, f, 'trace_wram 4 4')
        cmd(s, f, 'emu_wram_trace_add 4 4')

        # Step both 1 frame
        cmd(s, f, 'step 1')
        cmd(s, f, 'emu_step 1')

        # Read both traces
        rt = cmd(s, f, 'get_wram_trace').get('log', [])
        ot = cmd(s, f, 'emu_get_wram_trace').get('log', [])

        print(f'=== recomp writes to $04 in first frame of GM=0x07 ({len(rt)}) ===')
        for e in rt[-30:]:
            print(f'  f{e["f"]:4} {e["adr"]} {e["old"]}->{e["val"]} '
                  f'func={e.get("func", "?"):60s} parent={e.get("parent", "?")}')
        print(f'\n=== oracle writes to $04 ({len(ot)}) ===')
        for e in ot[-30:]:
            print(f'  f{e["f"]:4} {e["adr"]} {e.get("before","?")}->{e["after"]} '
                  f'pc={e.get("pc","?")} bank={e.get("bank_src","?")}')
    finally:
        s.close()
        p.terminate()
        try: p.wait(timeout=5)
        except subprocess.TimeoutExpired: p.kill()
        subprocess.run(['taskkill','/F','/IM','smw.exe'],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

if __name__ == '__main__':
    main()
