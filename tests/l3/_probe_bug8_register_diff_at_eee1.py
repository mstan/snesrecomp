"""Bug #8 phase 14: at $00:EEE1 (entry of RunPlayerBlockCode_00EEE1),
dump A/X/Y on both sides and compare.

Static analysis showed v13 (the value stored to $7D at $EF60) depends
on the Y argument indexing into ROM tables $E4FB or $E4DA. If recomp
and emu enter $EEE1 with different Y, the divergence is upstream of
$EEE1 — find which caller passes the wrong Y. If both enter with the
SAME Y, the divergence is INSIDE $EEE1's block chain (worse).

This probe: arm a recomp breakpoint at $EEE1; let recomp tick; on the
first hit, dump get_cpu_state. Concurrently, drive emu to a comparable
PC=$EEE1 hit (snes9x doesn't have a clean break primitive in our
bridge, but emu_cpu_regs at the right moment + emu_wram_trace_add at
$EEE1's effects gives equivalent info).
"""
import sys, pathlib, time, subprocess, socket
THIS_DIR = pathlib.Path(__file__).parent
sys.path.insert(0, str(THIS_DIR))
from harness import DebugClient, RECOMP_PORT  # noqa: E402

REPO = pathlib.Path(__file__).parent.parent.parent.parent
ORACLE_EXE = REPO / 'build' / 'bin-x64-Oracle' / 'smw.exe'


def _kill():
    subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'], capture_output=True)


def launch():
    _kill(); time.sleep(0.5)
    subprocess.Popen([str(ORACLE_EXE), '--paused'],
                     cwd=str(REPO),
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    deadline = time.time() + 20
    while time.time() < deadline:
        try:
            s = socket.create_connection(('127.0.0.1', RECOMP_PORT), timeout=0.3)
            s.close(); return
        except OSError:
            time.sleep(0.2)
    raise RuntimeError('no TCP connect')


def step_to(c, target):
    cur = c.cmd('frame').get('frame', 0)
    if cur >= target: return cur
    c.cmd(f'step {target - cur}')
    deadline = time.time() + 60
    while time.time() < deadline:
        if c.cmd('frame').get('frame', 0) >= target: return target
        time.sleep(0.03)


def wait_parked(c, max_s=10):
    deadline = time.time() + max_s
    while time.time() < deadline:
        r = c.cmd('parked')
        if r.get('parked'): return r
        time.sleep(0.02)
    return None


def main():
    launch()
    c = DebugClient(RECOMP_PORT)
    try:
        c.cmd('pause')

        # Step recomp to f205 (deep into the gravity-accumulation window
        # where the bug manifests every frame).
        step_to(c, 205)
        print(f'Recomp at f205, GameMode=0x{int(c.cmd("read_ram 0x100 1").get("hex","0").replace(" ","")[:2],16):02x}')

        # Arm a breakpoint at $EEE1 entry. Then continue and wait.
        c.cmd('break_clear')
        r = c.cmd('break_add eee1')
        print(f'break_add eee1: {r}')
        c.cmd('continue')
        parked = wait_parked(c, 10)
        if not parked:
            print('TIMEOUT — no break hit at $EEE1')
            return
        print(f'\nRecomp parked at: {parked}')

        # Dump recomp register state at the break.
        cpu = c.cmd('get_cpu_state')
        print(f'\nRecomp CPU at $EEE1:')
        print(f'  {cpu}')

        # Also dump key WRAM that the function reads at entry:
        for label, addr, w in [
            ('Y arg (ABI register)',  None, 0),
            ('PlayerXSpeed+1 ($7B)',  0x7B, 1),
            ('PlayerYSpeed+1 ($7D)',  0x7D, 1),
            ('TempScreenMode ($8E)',  0x8E, 1),
            ('Map16TileNumber ($9C)', 0x9C, 1),
            ('PlayerXPosNext ($94)',  0x94, 2),
            ('PlayerYPosNext ($96)',  0x96, 2),
        ]:
            if addr is None: continue
            r = c.cmd(f'read_ram 0x{addr:x} {w}')
            h = r.get('hex','').replace(' ','')
            v = int.from_bytes(bytes.fromhex(h)[:w],'little') if h else None
            print(f'  recomp {label:<24} = 0x{v:0{w*2}x}' if v is not None else f'  {label} = ??')

        # Now drive emu to its equivalent state. Use the watch on $7D
        # to detect when emu hits $EF62 (the STA inside $EEE1's chain).
        # When that fires, we know emu just executed $EEE1 -> ... -> $EF60.
        # Then read emu_cpu_regs at that moment.
        print('\nNow driving emu... (this side is harder — no direct emu-PC-break,')
        print('but emu_get_wram_trace gives PC at write events.)')

        # Snap emu state RIGHT NOW (the recomp is still parked so emu has
        # been free-running via per-recomp-frame ticks until f205).
        # Reset trace and step a few emu frames to capture EEE1-related writes.
        c.cmd('emu_wram_trace_reset')
        c.cmd('emu_wram_trace_add 7d 7d')
        c.cmd('emu_step 1')
        r = c.cmd('emu_get_wram_trace')
        log = r.get('log', [])
        print(f'\nEmu writes to $7D in next 1 emu-frame ({len(log)} writes):')
        for e in log:
            print(f'  PC={e.get("pc")}  before={e.get("before")} after={e.get("after")}')

        # Read emu's WRAM bytes that correspond to RunPlayerBlockCode_00EEE1's
        # input dependencies at the moment after the frame.
        print(f'\nEmu state (post-1-frame-step):')
        for label, addr, w in [
            ('PlayerXSpeed+1 ($7B)',  0x7B, 1),
            ('PlayerYSpeed+1 ($7D)',  0x7D, 1),
            ('TempScreenMode ($8E)',  0x8E, 1),
            ('Map16TileNumber ($9C)', 0x9C, 1),
            ('PlayerXPosNext ($94)',  0x94, 2),
            ('PlayerYPosNext ($96)',  0x96, 2),
        ]:
            r = c.cmd(f'emu_read_wram 0x{addr:x} {w}')
            h = r.get('hex','')
            v = int.from_bytes(bytes.fromhex(h)[:w],'little') if h else None
            print(f'  emu {label:<24} = 0x{v:0{w*2}x}' if v is not None else f'  emu {label} = ??')

        # Also dump emu CPU regs (will be at NMI vector since frame just ended).
        r = c.cmd('emu_cpu_regs')
        print(f'\nEmu CPU regs (post-frame, at NMI handler):  {r}')

        c.cmd('break_clear')
        c.cmd('continue')
    finally:
        c.close(); _kill()


if __name__ == '__main__':
    main()
