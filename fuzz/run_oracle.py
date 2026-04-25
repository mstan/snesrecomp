"""Oracle-side runner for Phase B differential fuzz.

Launches the Oracle|x64 build of smw.exe paused, connects to its
TCP debug server, sends one `fuzz_run_snippet` command per snippet,
collects the WRAM snapshot after execution, diffs against the same
baseline the recomp runner uses, and writes a JSONL of results
parallel to results/recomp_final.jsonl.

Round-trip cost: one TCP exchange per snippet. 1189 snippets at
~1 ms each = ~1 s. Acceptable for initial-pass dev.
"""
from __future__ import annotations
import json
import pathlib
import socket
import subprocess
import sys
import time

FUZZ_DIR = pathlib.Path(__file__).resolve().parent
# Parent project root: FUZZ_DIR=snesrecomp/fuzz, so parents[2] is parent of
# snesrecomp/. Via junction layout this is the parent repo (F:/Projects/
# SuperMarioWorldRecomp), not F:/Projects/snesrecomp.
REPO = pathlib.Path(r'F:/Projects/SuperMarioWorldRecomp')
ORACLE_EXE = REPO / 'build/bin-x64-Oracle/smw.exe'
SNIPPETS = FUZZ_DIR / 'snippets' / 'snippets.json'
OUT_DIR = FUZZ_DIR / 'results'
OUT_DIR.mkdir(exist_ok=True)


BASELINE = {
    0x10: 0x55, 0x11: 0xAA, 0x100: 0x33, 0x101: 0xCC,
    # Indirect-mode pointer at $20-$22; LONG target at $200.
    0x20: 0x00, 0x21: 0x01, 0x22: 0x00,
    0x200: 0x77, 0x201: 0x88,
    # Flag-capture slots pre-seeded to 0xFF; conditional STZ writes 0
    # when the corresponding flag is set. See generate_snippets.py.
    0x1F06: 0xFF, 0x1F07: 0xFF, 0x1F08: 0xFF, 0x1F09: 0xFF,
}


def compute_initial_cpu(seed: dict, m_flag: int, x_flag: int) -> dict:
    """Translate a snippet's seed dict into full CPU register seed
    for the oracle. The recomp snippet prologue does REP/SEP + LDA/LDX/LDY
    etc. to set these in the emulation itself, so for the ORACLE side we
    just need a safe initial state — the snippet's prologue will set the
    actual test-relevant values.
    """
    # P byte layout: NVMXDIZC. Start with M=1 X=1 I=1 (interrupts off).
    # The snippet's REP/SEP prologue will change M/X as needed.
    p = 0x34  # M=1, X=1, I=1 (0b00110100)
    return {
        'a': 0, 'x': 0, 'y': 0,
        's': 0x1FF, 'd': 0,
        'db': 0x00, 'p': p,
    }


def connect_oracle(exe_path: pathlib.Path) -> tuple:
    subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.3)
    p = subprocess.Popen([str(exe_path), '--paused'],
                         cwd=str(REPO),
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    s = socket.socket()
    for _ in range(50):
        try:
            s.connect(('127.0.0.1', 4377)); break
        except (ConnectionRefusedError, OSError):
            time.sleep(0.2)
    f = s.makefile('r')
    f.readline()  # banner
    return p, s, f


def send_cmd(s: socket.socket, f, line: str) -> dict:
    s.sendall((line + '\n').encode())
    resp_line = f.readline()
    if not resp_line:
        raise RuntimeError('TCP closed')
    return json.loads(resp_line)


def wram_delta(wram_hex: str) -> dict:
    """Parse a hex blob of WRAM $0000-$1FFF and return {addr: byte} for
    bytes differing from the baseline."""
    data = bytes.fromhex(wram_hex)
    out = {}
    for i, b in enumerate(data):
        baseline = BASELINE.get(i, 0)
        if b != baseline:
            out[f'0x{i:x}'] = b
    return out


def main():
    snippets = json.load(open(SNIPPETS))
    print(f'running {len(snippets)} snippets through oracle backend...',
          file=sys.stderr)

    if not ORACLE_EXE.exists():
        print(f'ERROR: Oracle exe not found: {ORACLE_EXE}', file=sys.stderr)
        print(f'       Build Oracle|x64 first.', file=sys.stderr)
        raise SystemExit(2)

    proc, sock, f = connect_oracle(ORACLE_EXE)
    out_path = OUT_DIR / 'oracle_final.jsonl'
    ok = 0
    err = 0
    t0 = time.time()
    try:
        with open(out_path, 'w') as out_f:
            for s in snippets:
                seed = compute_initial_cpu(s['seed'], s['m_flag'], s['x_flag'])
                cmd = (f'fuzz_run_snippet {s["rom_hex"]} '
                       f'{seed["a"]} {seed["x"]} {seed["y"]} '
                       f'{seed["s"]} {seed["d"]} '
                       f'{seed["db"]} {seed["p"]}')
                try:
                    resp = send_cmd(sock, f, cmd)
                except Exception as e:
                    out_f.write(json.dumps({'id': s['id'], 'error': str(e)}) + '\n')
                    err += 1
                    continue
                if not resp.get('ok'):
                    out_f.write(json.dumps({'id': s['id'], 'error': resp.get('error', 'unknown')}) + '\n')
                    err += 1
                    continue
                delta = wram_delta(resp['wram_hex'])
                out_f.write(json.dumps({'id': s['id'], 'wram_delta': delta}) + '\n')
                ok += 1
    finally:
        sock.close()
        proc.terminate()
        try: proc.wait(timeout=3)
        except subprocess.TimeoutExpired: proc.kill()
        subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    dt = time.time() - t0
    print(f'wrote {out_path} — {ok} ok, {err} err, {dt:.1f}s', file=sys.stderr)


if __name__ == '__main__':
    main()
