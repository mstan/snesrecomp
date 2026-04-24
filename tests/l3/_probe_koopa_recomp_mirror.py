"""Koopa-falls step 2b: mirror-test the oracle caller chain on recomp.

Oracle's path into $019A04 (SetSomeYSpeed__) for the walking koopa is:
  Spr0to13Main  ($018B0A)
    -> $018B46  JSR SubUpdateSprPos
    -> $018B49  JSR SetAnimationFrame
    -> $018B4C  JSR IsOnGround
    -> $018B4F  BEQ SpriteInAir  (not taken — on ground)
    -> $018B51  JSR SetSomeYSpeed__

Test each PC on recomp with break_add. The first MISS after a HIT
identifies where recomp diverges from oracle's in-function flow.

If $018B0A itself MISSES -> Spr0to13Main is never called by recomp's
sprite dispatcher. Diagnose the dispatcher instead.
If $018B0A HITS but $018B4F's BEQ takes a different branch -> the
on-ground test differs. Compare IsOnGround inputs/outputs."""
from __future__ import annotations
import json, pathlib, socket, subprocess, sys, time

REPO = pathlib.Path(r'F:/Projects/SuperMarioWorldRecomp')
EXE = REPO / 'build/bin-x64-Oracle/smw.exe'

TESTS = [
    (0x018B0A, 'Spr0to13Main entry'),
    (0x018B43, 'CODE_018B43 (after BEQ CODE_018B43 at $018B3E; also SubOffscreen)'),
    (0x018B46, 'JSR SubUpdateSprPos'),
    (0x018B49, 'JSR SetAnimationFrame'),
    (0x018B4C, 'JSR IsOnGround'),
    (0x018B4F, 'BEQ SpriteInAir'),
    (0x018B51, 'JSR SetSomeYSpeed__'),
    (0x019A04, 'SetSomeYSpeed__ entry (handoff said MISS)'),
]


def cmd(s, f, l):
    s.sendall((l + '\n').encode()); return json.loads(f.readline())


def test_break(pc, label):
    subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.5)
    p = subprocess.Popen([str(EXE), '--paused'],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, cwd=str(REPO))
    try:
        s = socket.socket()
        for _ in range(50):
            try: s.connect(('127.0.0.1', 4377)); break
            except (ConnectionRefusedError, OSError): time.sleep(0.2)
        f = s.makefile('r'); f.readline()
        cmd(s, f, f'break_add 0x{pc:x}')
        cmd(s, f, 'step 400')
        time.sleep(3.0)
        for _ in range(30):
            r = cmd(s, f, 'parked')
            if r.get('parked'):
                fr = cmd(s, f, 'frame').get('frame')
                stack_depth = r.get('stack_depth', '?')
                stack = r.get('stack', [])
                cmd(s, f, 'break_continue')
                return f'HIT  @ f{fr} sd={stack_depth} top={stack[:3] if stack else []}'
            time.sleep(0.2)
        return 'MISS'
    finally:
        try: s.close()
        except Exception: pass
        p.terminate()
        try: p.wait(timeout=5)
        except subprocess.TimeoutExpired: p.kill()
        subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def main():
    for pc, label in TESTS:
        result = test_break(pc, label)
        print(f'${pc:06x} {label:50s} {result}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
