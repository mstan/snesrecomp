"""Arm break_add 0xee3a on recomp, advance 200 frames, check if it
ever parks. If yes: EE3A was reached. If no: recomp never executes
$EE3A in the boot window."""
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
        r = cmd(s, f, 'break_add 0xee3a')
        print(f'break armed: {r}')
        cmd(s, f, 'step 400')
        time.sleep(3.0)
        for _ in range(20):
            r = cmd(s, f, 'parked')
            if r.get('parked'):
                print(f'\nPARKED on $EE3A: {r}')
                print(f'frame: {cmd(s, f, "frame").get("frame")}')
                # free it
                cmd(s, f, 'break_continue')
                return 0
            time.sleep(0.2)
        print(f'\nnever parked after step 400 -> EE3A never reached in recomp boot')
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
