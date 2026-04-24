"""Koopa-falls step 5: insn-trace recomp's path through
HandleNormalSpriteLevelCollision ($019140) and follow whether it
reaches CODE_019435 ($019435 — the on-ground flag setter).

Oracle reaches $019435 via JSR at $019414.
If recomp doesn't reach $019414 -> divergence is earlier in the
collision routine. If it reaches $019414 but not $019435 -> the JSR
isn't being emitted. If it reaches $019435 but the write is wrong,
the bug is in the bit-set logic."""
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

        cmd(sock, f, 'trace_insn_reset')
        cmd(sock, f, 'trace_insn')
        cmd(sock, f, 'step 400')
        deadline = time.time() + 60
        while time.time() < deadline:
            fr = cmd(sock, f, 'frame').get('frame', 0)
            if fr >= 380: break
            time.sleep(0.2)
        time.sleep(2.0)
        print(f'recomp frame: {cmd(sock, f, "frame").get("frame")}')

        # Wider region covering CODE_019523 + following Map16TileNumber STA
        print('\n=== $019100-$019540 — frame count per frame ===')
        r = cmd(sock, f, 'get_insn_trace pc_lo=0x019100 pc_hi=0x019540 limit=4096')
        hits = r.get('log', [])
        per_frame = {}
        for e in hits:
            per_frame.setdefault(e['f'], 0)
            per_frame[e['f']] += 1
        for fr in sorted(per_frame):
            print(f'  f{fr}: {per_frame[fr]} insns')
        # Show frame 201 (first frame when physics resumed)
        f201 = [e for e in hits if e['f'] == 201]
        f95 = [e for e in hits if e['f'] == 95]
        print(f'\n  frame 95 hits: {len(f95)}')
        print(f'  frame 201 hits: {len(f201)}')
        print('  --- f95 context around $0192DE BEQ (target: $01930F or fall-through to $019310) ---')
        for i, e in enumerate(f95):
            pc = int(e['pc'], 16)
            if 0x0192da <= pc <= 0x019310:
                mn = MNEM[e['mnem']] if e['mnem'] < len(MNEM) else '?'
                print(f'    bi={e["bi"]:9} pc={e["pc"]} {mn:4} '
                      f'a={e["a"]} x={e["x"]} y={e["y"]}')
        print('  --- f201 context around $0192DE BEQ ---')
        for i, e in enumerate(f201):
            pc = int(e['pc'], 16)
            if 0x0192da <= pc <= 0x019310:
                mn = MNEM[e['mnem']] if e['mnem'] < len(MNEM) else '?'
                print(f'    bi={e["bi"]:9} pc={e["pc"]} {mn:4} '
                      f'a={e["a"]} x={e["x"]} y={e["y"]}')
        print('  --- f201 PCs (last 80) ---')
        for e in f201[-80:]:
            mn = MNEM[e['mnem']] if e['mnem'] < len(MNEM) else '?'
            print(f'  bi={e["bi"]:9} pc={e["pc"]} {mn:4} '
                  f'a={e["a"]} x={e["x"]} y={e["y"]} b={e["b"]} m={e["m"]} xf={e["xf"]}')

        # Any hits at $019435 (sub_019435 entry)?
        print('\n=== $019435 (on-ground flag setter) ===')
        r = cmd(sock, f, 'get_insn_trace pc_lo=0x019435 pc_hi=0x019440 limit=4096')
        hits = r.get('log', [])
        print(f'  total hits: {len(hits)}')
        for e in hits[:20]:
            mn = MNEM[e['mnem']] if e['mnem'] < len(MNEM) else '?'
            print(f'  f{e["f"]} bi={e["bi"]:9} pc={e["pc"]} {mn:4} '
                  f'a={e["a"]} x={e["x"]} y={e["y"]} b={e["b"]}')
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
