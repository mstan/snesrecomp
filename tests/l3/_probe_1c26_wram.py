"""Dump WRAM $1C00-$1D00 on both sides at f96 to find the palette-bit diff."""
import sys, pathlib, time, subprocess, socket
THIS_DIR = pathlib.Path(__file__).parent
sys.path.insert(0, str(THIS_DIR))
from harness import RECOMP_EXE, ORACLE_EXE, RECOMP_PORT, ORACLE_PORT, DebugClient  # noqa: E402


def _kill(): subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'], capture_output=True)
def _ports_ready():
    for p in (RECOMP_PORT, ORACLE_PORT):
        try:
            s = socket.create_connection(('127.0.0.1', p), timeout=0.3); s.close()
        except OSError: return False
    return True
def launch_both():
    _kill(); time.sleep(0.5)
    subprocess.Popen([str(RECOMP_EXE), '--paused'],
        cwd=str(RECOMP_EXE.parent.parent.parent),
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.Popen([str(ORACLE_EXE), '--paused', '--theirs'],
        cwd=str(ORACLE_EXE.parent.parent.parent),
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    deadline = time.time() + 15
    while time.time() < deadline:
        if _ports_ready(): time.sleep(0.3); return
        time.sleep(0.2)
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


def read_bytes(c, addr, n):
    r = c.cmd(f'read_ram 0x{addr:x} {n}')
    hexs = r.get('hex', '').replace(' ', '')
    return bytes.fromhex(hexs) if hexs else b''


def main():
    launch_both()
    r = DebugClient(RECOMP_PORT); o = DebugClient(ORACLE_PORT)
    try:
        r.cmd('pause'); o.cmd('pause')
        for target in [94, 95, 96]:
            step_to(r, target); step_to(o, target)
            print(f'\n=== FRAME {target} ===')
            # Dump $1BE0-$1D00 (Layer1 + Layer2 DMA data buffers)
            for base, name in [(0xBAB0, 'Map16LowPtr target $BAB0+'),
                              (0xBE00, 'Map16HighPtr target $BE00+ (+0x50)')]:
                rb = read_bytes(r, base, 0x40)
                ob = read_bytes(o, base, 0x40)
                if rb != ob:
                    print(f'\n{name} @ ${base:04x} DIFFERS:')
                    for i in range(0, len(rb), 16):
                        rline = ' '.join(f'{b:02x}' for b in rb[i:i+16])
                        oline = ' '.join(f'{b:02x}' for b in ob[i:i+16])
                        if rb[i:i+16] != ob[i:i+16]:
                            marker = ' <-- DIFF'
                            diff_bytes = [j for j in range(16) if rb[i+j] != ob[i+j]]
                            print(f'  ${base+i:04x} R: {rline}{marker} (bytes {diff_bytes})')
                            print(f'  ${base+i:04x} O: {oline}')
                        else:
                            print(f'  ${base+i:04x}   {rline}')
                else:
                    print(f'\n{name} @ ${base:04x}: identical ({len(rb)} bytes)')
    finally:
        r.close(); o.close(); _kill()


if __name__ == '__main__':
    main()
