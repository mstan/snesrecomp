"""Koopa-falls step 2: check if SetNormalSpriteYSpeedBasedOnSlope
(ROM $019A04) is ever called on recomp. Oracle writes YSpeed[9]=0
via this function's STA at $019A12. Recomp has no such reset.

If break_add on $019A04 fires on recomp → function IS reached, but
stores wrong value (maybe X=wrong slot, or A=wrong value).
If break doesn't fire → caller doesn't reach this function on recomp.
"""
import json, pathlib, socket, subprocess, sys, time
REPO = pathlib.Path(r'F:/Projects/SuperMarioWorldRecomp')
EXE = REPO / 'build/bin-x64-Oracle/smw.exe'
def cmd(s, f, l):
    s.sendall((l + '\n').encode()); return json.loads(f.readline())
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
        r = cmd(s, f, 'break_add 0x19a04')
        print(f'break armed: {r}')
        cmd(s, f, 'step 400')
        time.sleep(3.0)
        for _ in range(25):
            r = cmd(s, f, 'parked')
            if r.get('parked'):
                print(f'\nPARKED: {r}')
                print(f'frame: {cmd(s, f, "frame").get("frame")}')
                cmd(s, f, 'break_continue')
                return 0
            time.sleep(0.2)
        print('\n$019A04 NOT reached — SetNormalSpriteYSpeedBasedOnSlope never called')
        return 1
    finally:
        try: s.close()
        except Exception: pass
        p.terminate()
        try: p.wait(timeout=5)
        except subprocess.TimeoutExpired: p.kill()
        subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

if __name__ == '__main__':
    sys.exit(main())
