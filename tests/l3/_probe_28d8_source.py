"""Find which DMA writes $V28d8 and identify its source WRAM address.

Plan: step to f95, trace_reg + trace_vram both across f96, then match
the VRAM write at $V28d8 to the most recent $420B trigger that
configured a DMA targeting that VMADD. That DMA's $4312/$4313/$4314
give src bank:addr. Read src WRAM on both sides.
"""
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


def main():
    launch_both()
    r = DebugClient(RECOMP_PORT); o = DebugClient(ORACLE_PORT)
    try:
        r.cmd('pause'); o.cmd('pause')
        step_to(r, 95); step_to(o, 95)
        r.cmd('trace_vram_reset'); o.cmd('trace_vram_reset')
        r.cmd('trace_reg_reset'); o.cmd('trace_reg_reset')
        # Very narrow VRAM trace around the first diff
        r.cmd('trace_vram 28d8 28d8'); o.cmd('trace_vram 28d8 28d8')
        # Full DMA ch param trace
        r.cmd('trace_reg 2115 2117'); o.cmd('trace_reg 2115 2117')
        r.cmd('trace_reg 4300 4317'); o.cmd('trace_reg 4300 4317')
        r.cmd('trace_reg 420b 420b'); o.cmd('trace_reg 420b 420b')
        step_to(r, 96); step_to(o, 96)

        rv = r.cmd('get_vram_trace nostack').get('log', [])
        ov = o.cmd('get_vram_trace nostack').get('log', [])
        print(f'VRAM writes to $V28d8: recomp={len(rv)} oracle={len(ov)}')
        for side, log in [('R', rv), ('O', ov)]:
            for e in log:
                print(f'  [{side}] f{e["f"]} ${e["adr"]}={e["val"]}')

        rt = r.cmd('get_reg_trace nostack').get('log', [])
        ot = o.cmd('get_reg_trace nostack').get('log', [])
        # Walk backwards: find the DMA whose VMADD corresponds to 0x28d8 that
        # triggered BEFORE each VRAM write at 0x28d8. DMAs at VMADD=0x28XX
        # with VMAIN=0x81 (stride 32) reach 0x28d8 if starting addr walks
        # through that col/row. Approximate: find last DMA trigger before
        # each 0x28d8 VRAM write.
        def find_dma_before_vram_write(reglog, vramwrites):
            # reconstruct DMA triggers with their VMADD/count/src
            cur = {}
            dmas = []
            for e in reglog:
                adr = int(e['adr'], 16); val = int(e['val'], 16)
                cur[adr] = val
                if adr == 0x420b:
                    vmadd = (cur.get(0x2117, 0) << 8) | cur.get(0x2116, 0)
                    count = (cur.get(0x4316, 0) << 8) | cur.get(0x4315, 0)
                    src   = (cur.get(0x4314, 0) << 16) | (cur.get(0x4313, 0) << 8) | cur.get(0x4312, 0)
                    dmas.append({'vmadd': vmadd, 'count': count, 'src': src,
                                 'trigger': val, 'func': e.get('func', ''),
                                 'idx': len([x for x in reglog[:reglog.index(e)+1]])})
            # For each VRAM write, find the CLOSEST preceding DMA whose
            # target range would have reached the VRAM addr.
            for vw in vramwrites:
                vadr = int(vw['adr'], 16); vval = int(vw['val'], 16)
                # Match DMA to 0x28d8: VMADD writes with VMAIN=0x81 walk from
                # VMADD to VMADD+(count/2)*0x20-0x20 with stride 0x20 on high.
                # So 0x28d8 belongs to DMAs with VMADD=0x28d8, 0x28d8-0x20,
                # 0x28d8-0x40, etc. (same col, different starting rows).
                # Col of 0x28d8 = 0x18, row = 6. Anything with low byte matching
                # (0x28d8 & 0x1f) = 0x18 and same row-or-earlier-and-reaches.
                candidates = []
                for d in dmas:
                    col_match = (d['vmadd'] & 0x1f) == (vadr & 0x1f)
                    if not col_match: continue
                    # If VMADD <= vadr and VMADD + (count/2)*0x20 - 0x20 >= vadr
                    if d['vmadd'] > vadr: continue
                    reach = d['vmadd'] + (d['count']//2 - 1) * 0x20
                    if reach < vadr: continue
                    candidates.append(d)
                if candidates:
                    match = candidates[-1]  # most recent matching
                    print(f'  VRAM write ${vadr:04x}=${vval:04x} came from DMA '
                          f'vmadd=${match["vmadd"]:04x} count=${match["count"]:04x} '
                          f'src=${match["src"]:06x} func={match["func"]}')
                else:
                    print(f'  VRAM write ${vadr:04x}=${vval:04x} -- no DMA match found')
        print('\n=== RECOMP DMA matches ===')
        find_dma_before_vram_write(rt, rv)
        print('\n=== ORACLE DMA matches ===')
        find_dma_before_vram_write(ot, ov)
    finally:
        r.close(); o.close(); _kill()


if __name__ == '__main__':
    main()
