"""Dump $V2800-$V2FFF on recomp and oracle at selected frames. Count
non-zero words (informal "has data").
"""
import sys, pathlib, time, subprocess, socket
THIS_DIR = pathlib.Path(__file__).parent
sys.path.insert(0, str(THIS_DIR))
from harness import RECOMP_EXE, ORACLE_EXE, RECOMP_PORT, ORACLE_PORT, DebugClient  # noqa: E402


def _kill():
    subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'], capture_output=True)


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


def dump_vram_region(c, lo_word, hi_word):
    # dump_vram byte_start byte_len
    r = c.cmd(f'dump_vram {lo_word*2:x} {(hi_word-lo_word+1)*2:x}')
    hex_str = r.get('hex', '')
    data = bytes.fromhex(hex_str)
    words = []
    for i in range(0, len(data), 2):
        words.append(data[i] | (data[i+1] << 8))
    return words


def main():
    launch_both()
    r = DebugClient(RECOMP_PORT); o = DebugClient(ORACLE_PORT)
    try:
        r.cmd('pause'); o.cmd('pause')
        for target in [94, 95, 96, 100]:
            step_to(r, target); step_to(o, target)
            rwords = dump_vram_region(r, 0x2800, 0x2fff)
            owords = dump_vram_region(o, 0x2800, 0x2fff)
            r_nonzero = sum(1 for w in rwords if w != 0)
            o_nonzero = sum(1 for w in owords if w != 0)
            diff = sum(1 for a, b in zip(rwords, owords) if a != b)
            print(f'f{target}: $V2800-$V2FFF  recomp_nonzero={r_nonzero}/2048  oracle_nonzero={o_nonzero}/2048  diff_words={diff}')
            if diff > 0:
                diffs = [(0x2800+i, rwords[i], owords[i]) for i in range(len(rwords)) if rwords[i]!=owords[i]]
                print(f'   first 25 diffs: (addr, recomp, oracle)')
                for a, r_, o_ in diffs[:25]:
                    print(f'     ${a:04x}: recomp=0x{r_:04x} oracle=0x{o_:04x}')
        # Also dump $V6000 region
        print()
        for target in [94, 95, 96, 100]:
            step_to(r, target); step_to(o, target)
            rwords = dump_vram_region(r, 0x6000, 0x67ff)
            owords = dump_vram_region(o, 0x6000, 0x67ff)
            r_nonzero = sum(1 for w in rwords if w != 0)
            o_nonzero = sum(1 for w in owords if w != 0)
            diff = sum(1 for a, b in zip(rwords, owords) if a != b)
            print(f'f{target}: $V6000-$V67FF  recomp_nonzero={r_nonzero}/2048  oracle_nonzero={o_nonzero}/2048  diff_words={diff}')
    finally:
        r.close(); o.close(); _kill()


if __name__ == '__main__':
    main()
