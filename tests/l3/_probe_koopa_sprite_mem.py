"""Frozen koopa step 3: check SpriteMemorySetting ($1692) and key
sprite-spawn registers at f94 on recomp vs oracle equivalent moment."""
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
def r(s, f, addr): return int(cmd(s, f, f'dump_ram 0x{addr:x} 1')['hex'].replace(' ',''), 16)
def e(s, f, addr):
    rr = cmd(s, f, f'emu_read_wram 0x{addr:x} 1')
    return int(rr['hex'].replace(' ', ''), 16) if rr.get('ok') else -1

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
        # Advance to recomp f95 (just past spawn in mode 0x03 -> 0x04 transition).
        for _ in range(95): step1(s, f)
        print('=== recomp @ f95 (just past spawn-time) ===')
        for addr, label in [(0x1692, 'SpriteMemorySetting'),
                            (0x1693, 'Map16TileNumber'),
                            (0x1933, 'BackgroundColor'),
                            (0x1931, 'ObjectTileset'),
                            (0x140d, 'SublevelLayer1MainPalette'),
                            (0x190b, 'CurrentLevel'),
                            (0x010b, 'CurrentSubLevel')]:
            print(f'  ${addr:04x} {label:25} = 0x{r(s,f,addr):02x}')
        # Catch oracle up to mode 0x04 (level just loaded).
        while e(s, f, 0x100) != 0x04:
            cmd(s, f, 'emu_step 1')
        # Oracle's spawn happens INSIDE mode 0x04 (between f241 and f296). After
        # mode 0x04 finishes, sprite slot 9 has the koopa. Stay in mode 0x04
        # for ~30 emu frames to be deep into the spawn.
        for _ in range(30):
            cmd(s, f, 'emu_step 1')
        print('\n=== oracle @ deep in mode 0x04 ===')
        for addr, label in [(0x1692, 'SpriteMemorySetting'),
                            (0x1693, 'Map16TileNumber'),
                            (0x1933, 'BackgroundColor'),
                            (0x1931, 'ObjectTileset'),
                            (0x140d, 'SublevelLayer1MainPalette'),
                            (0x190b, 'CurrentLevel'),
                            (0x010b, 'CurrentSubLevel')]:
            print(f'  ${addr:04x} {label:25} = 0x{e(s,f,addr):02x}')
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
