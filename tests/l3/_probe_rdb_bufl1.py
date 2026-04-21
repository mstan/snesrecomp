"""Tier-1 reverse-debugger litmus: invoke BufferScrollingTiles_Layer1 at
iter-32-state, capture every WRAM write via trace_wram, dump the full
sequence with per-write func attribution. If the divergent store is in
this dump, Tier 1 closes the ground bug.
"""
import sys, pathlib, time, subprocess, socket
THIS_DIR = pathlib.Path(__file__).parent
sys.path.insert(0, str(THIS_DIR))
from harness import RECOMP_EXE, RECOMP_PORT, DebugClient  # noqa: E402


def _kill(): subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'], capture_output=True)


def launch():
    _kill(); time.sleep(0.5)
    subprocess.Popen([str(RECOMP_EXE), '--paused'],
        cwd=str(RECOMP_EXE.parent.parent.parent),
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    deadline = time.time() + 15
    while time.time() < deadline:
        try:
            s = socket.create_connection(('127.0.0.1', RECOMP_PORT), timeout=0.3); s.close(); return
        except OSError: time.sleep(0.2)
    raise RuntimeError('no connect')


def step_to(c, target):
    base = c.cmd('frame').get('frame', 0)
    if base >= target: return base
    c.cmd(f'step {target - base}')
    deadline = time.time() + 60
    while time.time() < deadline:
        f = c.cmd('frame').get('frame', 0)
        if f >= target: return f
        time.sleep(0.05)
    return c.cmd('frame').get('frame', 0)


def rb(c, addr, n):
    r = c.cmd(f'read_ram 0x{addr:x} {n}')
    return bytes.fromhex(r.get('hex', '').replace(' ', ''))


def main():
    tileup = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0xFFF8
    s55 = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x02
    tiledown = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x17

    launch()
    c = DebugClient(RECOMP_PORT)
    try:
        c.cmd('pause')
        step_to(c, 95)

        # Force iter-32-like state
        c.cmd(f'write_ram 45 {tileup & 0xff:02x}')
        c.cmd(f'write_ram 46 {(tileup >> 8) & 0xff:02x}')
        c.cmd(f'write_ram 47 {tiledown & 0xff:02x}')
        c.cmd(f'write_ram 48 {(tiledown >> 8) & 0xff:02x}')
        c.cmd(f'write_ram 55 {s55:02x}')

        # Zero the L1 buffer so all changes are observable
        for off in range(0, 256):
            c.cmd(f'write_ram {0x1BE6 + off:x} 00')

        # Start Tier-1 WRAM trace on the L1 buffer + temps used by the fn
        r = c.cmd('trace_wram_reset')
        print(f'trace_wram_reset: {r}')
        r = c.cmd('trace_wram 1be6 1ce5')
        print(f'trace_wram L1Buffer: {r}')
        r = c.cmd('trace_wram 0 20')
        print(f'trace_wram $00-$20 (dp temps): {r}')
        r = c.cmd('trace_wram 6b 70')
        print(f'trace_wram Map16LowPtr/HighPtr: {r}')

        # Invoke
        invoked = c.cmd('invoke_recomp BufferScrollingTiles_Layer1')
        print(f'\nInvoke: {invoked}')

        # Dump trace
        trace = c.cmd('get_wram_trace')
        log = trace.get('log', [])
        print(f'\nCaptured {trace.get("entries")} writes (log payload has {len(log)})\n')

        # Print writes only to L1 buffer
        print(f'=== Writes to L1VramBuffer ($1BE6-$1CE5) ===')
        print(f'{"idx":>4}  {"addr":>6}  {"val":>6}  w')
        for i, e in enumerate(log):
            a = int(e['adr'], 16)
            if 0x1BE6 <= a <= 0x1CE5:
                print(f'{i:>4}  ${e["adr"]}  {e["val"]}  {e["w"]}')

        # Count unique tile words written to L1Buffer
        l1buf_words = {}
        for e in log:
            a = int(e['adr'], 16)
            if 0x1BE6 <= a <= 0x1CE5 and e['w'] == 2:
                l1buf_words[a] = int(e['val'], 16)
        print(f'\n=== L1VramBuffer unique 16-bit stores: {len(l1buf_words)} ===')
        nonblank = [a for a, v in l1buf_words.items() if v != 0x10F8]
        print(f'  non-blank (!= 0x10F8): {len(nonblank)}')
        for a in sorted(nonblank)[:30]:
            print(f'    ${a:04x} = 0x{l1buf_words[a]:04x}')
    finally:
        c.close(); _kill()


if __name__ == '__main__':
    main()
