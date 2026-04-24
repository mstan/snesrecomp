"""Koopa-falls step 3: Tier-4 instruction trace of recomp flowing
through Spr0to13Main ($018B0A..). Arm break at $018B0A, enable
trace_insn, resume a handful of frames, then dump all insn hits in
bank 01 PC range $018B00-$018BFF to see exactly where recomp's path
diverges from oracle.

Oracle path (from step 2a):
  $018B43 JSR SubOffscreen0Bnk1
  $018B46 JSR SubUpdateSprPos
  $018B49 JSR SetAnimationFrame
  $018B4C JSR IsOnGround ($01800E)
  $018B4F BEQ SpriteInAir  (NOT taken — koopa is on ground)
  $018B51 JSR SetSomeYSpeed__

If recomp's trace shows a branch at $018B4F that TAKES → IsOnGround
returned 0 on recomp → the on-ground test is where the gap lives."""
from __future__ import annotations
import json, pathlib, socket, subprocess, sys, time

REPO = pathlib.Path(r'F:/Projects/SuperMarioWorldRecomp')
EXE = REPO / 'build/bin-x64-Oracle/smw.exe'

MNEM = [
    "?","ADC","AND","ASL","BCC","BCS","BEQ","BIT","BMI","BNE",
    "BPL","BRA","BRK","BRL","BVC","BVS","CLC","CLD","CLI","CLV",
    "CMP","COP","CPX","CPY","DEC","DEX","DEY","EOR","INC","INX",
    "INY","JMP","JML","JSL","JSR","LDA","LDX","LDY","LSR","MVN",
    "MVP","NOP","ORA","PEA","PEI","PER","PHA","PHB","PHD","PHK",
    "PHP","PHX","PHY","PLA","PLB","PLD","PLP","PLX","PLY","REP",
    "ROL","ROR","RTI","RTL","RTS","SBC","SEC","SED","SEI","SEP",
    "STA","STP","STX","STY","STZ","TAX","TAY","TCD","TCS","TDC",
    "TRB","TSB","TSC","TSX","TXA","TXS","TXY","TYA","TYX","WAI",
    "WDM","XBA","XCE",
]


def cmd(sock, f, line):
    sock.sendall((line + '\n').encode()); return json.loads(f.readline())


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
        f = sock.makefile('r'); f.readline()

        # Arm trace_insn immediately, step to frame ~100 (well past
        # koopa spawn at f95), then pull any $018B**/$01800E hits.
        cmd(sock, f, 'trace_insn_reset')
        cmd(sock, f, 'trace_insn')
        cmd(sock, f, 'step 200')
        time.sleep(5.0)
        deadline = time.time() + 20
        while time.time() < deadline:
            fr = cmd(sock, f, 'frame').get('frame', 0)
            if fr >= 200: break
            time.sleep(0.2)
        print(f'recomp frame: {cmd(sock, f, "frame").get("frame")}')

        # Bank 01 has its own address space in 65816 trace. PCs are
        # 0x01XXXX.
        print('\n=== Spr0to13Main region $018B00-$018BFF ===')
        r = cmd(sock, f, 'get_insn_trace pc_lo=0x018b00 pc_hi=0x018bff limit=4096')
        for e in r.get('log', []):
            mn = MNEM[e['mnem']] if e['mnem'] < len(MNEM) else '?'
            print(f'  f{e["f"]:4} bi={e["bi"]:9} pc={e["pc"]} {mn:4} '
                  f'a={e["a"]} x={e["x"]} y={e["y"]} b={e["b"]} m={e["m"]} xf={e["xf"]}')

        print('\n=== IsOnGround $01800E-$018013 ===')
        r = cmd(sock, f, 'get_insn_trace pc_lo=0x01800e pc_hi=0x018013 limit=4096')
        for e in r.get('log', []):
            mn = MNEM[e['mnem']] if e['mnem'] < len(MNEM) else '?'
            print(f'  f{e["f"]:4} bi={e["bi"]:9} pc={e["pc"]} {mn:4} '
                  f'a={e["a"]} x={e["x"]} y={e["y"]} b={e["b"]}')

        print('\n=== SetSomeYSpeed__ $019A04-$019A14 (should be empty) ===')
        r = cmd(sock, f, 'get_insn_trace pc_lo=0x019a04 pc_hi=0x019a14 limit=4096')
        hits = r.get('log', [])
        print(f'  count: {len(hits)}')
        for e in hits[:5]:
            mn = MNEM[e['mnem']] if e['mnem'] < len(MNEM) else '?'
            print(f'  f{e["f"]:4} bi={e["bi"]:9} pc={e["pc"]} {mn:4}')
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
