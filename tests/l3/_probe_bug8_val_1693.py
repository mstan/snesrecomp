"""Check $1931 (ObjectTileset), $1693 (Map16TileNumber), and $7D at
frame 95 on recomp and equivalent moment on oracle."""
import json, pathlib, socket, subprocess, sys, time
REPO = pathlib.Path(r'F:/Projects/SuperMarioWorldRecomp')
EXE = REPO / 'build/bin-x64-Oracle/smw.exe'
def cmd(s, f, l):
    s.sendall((l + '\n').encode()); return json.loads(f.readline())
def step1(s, f):
    b = cmd(s, f, 'frame').get('frame', 0); cmd(s, f, 'step 1')
    dl = time.time() + 5
    while time.time() < dl:
        if cmd(s, f, 'frame').get('frame', 0) > b: return b + 1
        time.sleep(0.01)
def r(s, f, addr):
    return int(cmd(s, f, f'dump_ram 0x{addr:x} 1')['hex'].replace(' ',''), 16)
def e(s, f, addr):
    r = cmd(s, f, f'emu_read_wram 0x{addr:x} 1')
    return int(r['hex'].replace(' ', ''), 16) if r.get('ok') else -1
def main():
    subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.5)
    p = subprocess.Popen([str(EXE), '--paused'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, cwd=str(REPO))
    try:
        s = socket.socket()
        for _ in range(50):
            try: s.connect(('127.0.0.1', 4377)); break
            except (ConnectionRefusedError, OSError): time.sleep(0.2)
        f = s.makefile('r'); f.readline()
        # Advance recomp to frame 95
        for _ in range(95): step1(s, f)
        print(f'=== recomp @ f95 ===')
        print(f'  $1931 ObjectTileset = 0x{r(s,f,0x1931):02x}')
        print(f'  $1693 Map16TileNumber = 0x{r(s,f,0x1693):02x}')
        print(f'  $007D PlayerYSpeed_hi = 0x{r(s,f,0x7d):02x}')
        print(f'  $007C PlayerYSpeed_lo = 0x{r(s,f,0x7c):02x}')
        print(f'  $0090 PlayerYPosInBlock = 0x{r(s,f,0x90):02x}')
        print(f'  $0072 PlayerInAir = 0x{r(s,f,0x72):02x}')
        print(f'  $0100 GameMode = 0x{r(s,f,0x100):02x}')
        print(f'  $0096 PlayerYPosNext_lo = 0x{r(s,f,0x96):02x}')
        print(f'  $0097 PlayerYPosNext_hi = 0x{r(s,f,0x97):02x}')
        print(f'  $18B8 WallrunningType = 0x{r(s,f,0x18b8):02x}')

        # Advance oracle until it enters mode 0x07 then step backwards...
        # Actually simpler: advance oracle until right before its $72 clear.
        # Oracle's $72 clear was at oracle frame 296. Step oracle to ~295.
        while e(s,f,0x100) != 0x04:
            cmd(s,f,'emu_step 1')
        # Step oracle a few more to be deep in mode 0x04
        for _ in range(40):
            cmd(s,f,'emu_step 1')
        print(f'\n=== oracle @ deep in mode 0x04 ===')
        print(f'  $1931 ObjectTileset = 0x{e(s,f,0x1931):02x}')
        print(f'  $1693 Map16TileNumber = 0x{e(s,f,0x1693):02x}')
        print(f'  $007D PlayerYSpeed_hi = 0x{e(s,f,0x7d):02x}')
        print(f'  $007C PlayerYSpeed_lo = 0x{e(s,f,0x7c):02x}')
        print(f'  $0090 PlayerYPosInBlock = 0x{e(s,f,0x90):02x}')
        print(f'  $0072 PlayerInAir = 0x{e(s,f,0x72):02x}')
        print(f'  $0100 GameMode = 0x{e(s,f,0x100):02x}')
        print(f'  $0096 PlayerYPosNext_lo = 0x{e(s,f,0x96):02x}')
        print(f'  $0097 PlayerYPosNext_hi = 0x{e(s,f,0x97):02x}')
        print(f'  $18B8 WallrunningType = 0x{e(s,f,0x18b8):02x}')
        return 0
    finally:
        try: s.close()
        except Exception: pass
        p.terminate()
        try: p.wait(timeout=5)
        except subprocess.TimeoutExpired: p.kill()
        subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

if __name__ == '__main__':
    sys.exit(main())
