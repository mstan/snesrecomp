"""Bug #8 — RunPlayerBlockCode_00EE1D runs on both sides, but the
guard conditions decide which branch fires:
  $1471 StandOnSolidSprite  BEQ +  (if 0: goto label_ee2d, which sets $72=0x24 if unset)
  $7D   PlayerYSpeed+1      BMI +  (if negative: goto label_ee2d)
  else: JMP CODE_00EEE1 (tail call into the clearing chain).

If recomp has $1471=0 at frame 95 but oracle has $1471!=0 at oracle
frame 296, that's Bug #8's immediate cause. Compare both sides' state
at the critical frame."""
from __future__ import annotations
import json, pathlib, socket, subprocess, sys, time

REPO = pathlib.Path(r'F:/Projects/SuperMarioWorldRecomp')
EXE = REPO / 'build/bin-x64-Oracle/smw.exe'


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


def r_byte(sock, f, addr):
    r = cmd(sock, f, f'dump_ram 0x{addr:x} 1')
    return int(r['hex'].replace(' ', ''), 16)


def e_byte(sock, f, addr):
    r = cmd(sock, f, f'emu_read_wram 0x{addr:x} 1')
    return int(r['hex'].replace(' ', ''), 16) if r.get('ok') else -1


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

        # Watch $0100 for any write on recomp.
        # Also arm Tier 1 trace on $1471 and $7d to see their writes.
        cmd(sock, f, 'trace_wram_reset')
        cmd(sock, f, 'trace_wram 1471 1471')
        cmd(sock, f, 'emu_wram_trace_reset')
        cmd(sock, f, 'emu_wram_trace_add 1471 1471')

        # Advance to frame 100 (mode 0x04 just ran, we want f95 state).
        for _ in range(100):
            step1(sock, f)

        # Dump recomp state.
        print(f'=== recomp @ f{cmd(sock, f, "frame").get("frame")} ===')
        print(f'  $0100 GameMode        = 0x{r_byte(sock, f, 0x100):02x}')
        print(f'  $0072 PlayerInAir     = 0x{r_byte(sock, f, 0x72):02x}')
        print(f'  $007d PlayerYSpeed_hi = 0x{r_byte(sock, f, 0x7d):02x}')
        print(f'  $1471 StandOnSolidSprite = 0x{r_byte(sock, f, 0x1471):02x}')

        # Advance oracle separately until it reaches the equivalent state.
        # We want to compare oracle's state after its mode-0x04 handler ran.
        # Oracle's mode advances from $0100 timeline: 4->5 at oracle frame 296.
        # We need oracle to be in its mode-0x04 phase. Keep stepping oracle
        # only until its $0100 == 0x05 (to align post-mode-4).
        oframes = 0
        while e_byte(sock, f, 0x100) != 0x05 and oframes < 2000:
            cmd(sock, f, 'emu_step 1')
            oframes += 1
        print(f'\n=== oracle @ +{oframes} emu-only frames (just entered mode 0x05) ===')
        print(f'  $0100 GameMode        = 0x{e_byte(sock, f, 0x100):02x}')
        print(f'  $0072 PlayerInAir     = 0x{e_byte(sock, f, 0x72):02x}')
        print(f'  $007d PlayerYSpeed_hi = 0x{e_byte(sock, f, 0x7d):02x}')
        print(f'  $1471 StandOnSolidSprite = 0x{e_byte(sock, f, 0x1471):02x}')

        # Dump $1471 write history on both sides.
        print('\n=== recomp $1471 write history ===')
        r = cmd(sock, f, 'get_wram_trace')
        for e in r.get('log', [])[:20]:
            print(f'  f{e.get("f"):4} {e.get("old","?")}->{e.get("val")} '
                  f'func={e.get("func")} parent={e.get("parent")}')
        print('\n=== oracle $1471 write history ===')
        r = cmd(sock, f, 'emu_get_wram_trace')
        for e in r.get('log', [])[:20]:
            print(f'  f{e.get("f"):4} {e.get("before")}->{e.get("after")} '
                  f'pc={e.get("pc")} bank={e.get("bank_src")}')
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
