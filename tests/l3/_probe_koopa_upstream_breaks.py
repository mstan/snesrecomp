"""Koopa-falls step 3: bisect up the caller chain by arming breakpoints
at successive upstream PCs. The FIRST break that doesn't fire tells
us where recomp's flow diverges from oracle's expected path."""
import json, pathlib, socket, subprocess, sys, time
REPO = pathlib.Path(r'F:/Projects/SuperMarioWorldRecomp')
EXE = REPO / 'build/bin-x64-Oracle/smw.exe'

# PCs to test, in order of specificity (innermost first).
# 019A04 = SetNormalSpriteYSpeedBasedOnSlope entry
# 019969 = CODE_019969 (one caller site: JSR IsOnGround; BEQ; JSR SetSomeYSpeed__)
# 01A7DC = IsOnGround entry (common helper)
# 01A888 = approximate start of the sprite-update path that reaches 019969
TESTS = [
    (0x019a04, 'SetNormalSpriteYSpeedBasedOnSlope'),
    (0x019969, 'CODE_019969 (one JSR-IsOnGround site)'),
    (0x019984, 'CODE_019984 (fall-through from ground check)'),
]


def cmd(s, f, l):
    s.sendall((l + '\n').encode()); return json.loads(f.readline())


def test_break(pc, label):
    subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.5)
    p = subprocess.Popen([str(EXE), '--paused'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, cwd=str(REPO))
    try:
        s = socket.socket()
        for _ in range(50):
            try: s.connect(('127.0.0.1', 4377)); break
            except (ConnectionRefusedError, OSError): time.sleep(0.2)
        f = s.makefile('r'); f.readline()
        cmd(s, f, f'break_add 0x{pc:x}')
        cmd(s, f, 'step 300')
        time.sleep(2.0)
        for _ in range(20):
            r = cmd(s, f, 'parked')
            if r.get('parked'):
                fr = cmd(s, f, 'frame').get('frame')
                cmd(s, f, 'break_continue')
                return f'HIT  @ f{fr}'
            time.sleep(0.2)
        return 'MISS'
    finally:
        try: s.close()
        except Exception: pass
        p.terminate()
        try: p.wait(timeout=5)
        except subprocess.TimeoutExpired: p.kill()
        subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def main():
    for pc, label in TESTS:
        result = test_break(pc, label)
        print(f'${pc:06x} {label:45s} {result}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
