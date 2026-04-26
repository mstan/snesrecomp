"""Issue B (Mario sinks 1 tile under ground near Yoshi ?-block):
find the first frame the recomp's Mario-Y diverges from the snes9x
oracle, then dump the writer call chain on both sides for the
divergence frame.

Strategy
========
1. Boot the Oracle build (recomp + embedded snes9x), advance both
   sides to GM=0x07 (in-level gameplay).
2. Lockstep `step 1` + `emu_step 1` and sample the player Y bytes
   each frame: $0096/$0097 (PlayerYPosNext word), $00D3/$00D4
   (PlayerYPos word), $007D (player Y speed).
3. Report each frame as `MATCH` or `DIVERGE: rec=… emu=…`.
4. On first divergence, run trace_wram (recomp) +
   emu_wram_trace (emu) on the Y bytes for one extra frame to
   capture writer attribution.

The output identifies:
  * which Y byte diverges first (Y position, Y next, or Y speed)
  * the frame number
  * the function that wrote the diverging value on each side
This narrows the bug to a specific writer for a follow-up fix.

Run:  python snesrecomp/tests/l3/_probe_issue_b_mario_y.py
"""
from __future__ import annotations
import json, pathlib, socket, subprocess, sys, time

REPO = pathlib.Path(__file__).parent.parent.parent.parent
EXE = REPO / 'build' / 'bin-x64-Oracle' / 'smw.exe'
PORT = 4377

# Mario Y-state bytes we sample. Keep tight — every byte is a TCP
# round-trip per frame, and we step ~1000 frames.
SAMPLE = [
    (0x0096, 'YNextLo'),
    (0x0097, 'YNextHi'),
    (0x00D3, 'YPosLo'),
    (0x00D4, 'YPosHi'),
    (0x007D, 'YSpeed'),
]

# Frame ceiling. Yoshi spawns ~f900 in the demo per
# test_attract_demo_regression; Issue B (Mario near Yoshi block) must
# manifest before then. Cap at 1000 to leave margin without ballooning
# probe runtime.
MAX_FRAMES = 1000


def _kill():
    subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def cmd(sock, f, line):
    sock.sendall((line + '\n').encode())
    return json.loads(f.readline())


def _read_byte(sock, f, addr, side):
    if side == 'rec':
        r = cmd(sock, f, f'dump_ram 0x{addr:x} 1')
    else:
        r = cmd(sock, f, f'emu_read_wram 0x{addr:x} 1')
    h = r.get('hex', '').replace(' ', '')
    return int(h, 16) if h else -1


def _sample_all(sock, f, side):
    return {label: _read_byte(sock, f, a, side) for a, label in SAMPLE}


def _both_to_gm07(sock, f):
    """Advance recomp and emu independently to GM=$07 (in-level)."""
    rec_frames = 0
    for _ in range(3000):
        cmd(sock, f, 'step 1')
        rec_frames += 1
        if _read_byte(sock, f, 0x0100, 'rec') == 0x07:
            break
    emu_frames = 0
    for _ in range(3000):
        cmd(sock, f, 'emu_step 1')
        emu_frames += 1
        if _read_byte(sock, f, 0x0100, 'emu') == 0x07:
            break
    return rec_frames, emu_frames


def main():
    _kill(); time.sleep(0.5)
    proc = subprocess.Popen([str(EXE), '--paused'], cwd=str(REPO),
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    try:
        sock = socket.socket()
        for _ in range(60):
            try:
                sock.connect(('127.0.0.1', PORT)); break
            except (ConnectionRefusedError, OSError):
                time.sleep(0.2)
        else:
            raise RuntimeError('TCP connect timeout')
        f = sock.makefile('r'); f.readline()

        cmd(sock, f, 'pause')
        rf, ef = _both_to_gm07(sock, f)
        print(f'[init] recomp reached GM=07 after {rf} frames; '
              f'emu reached GM=07 after {ef} frames')

        # Snapshot at sync. If they already differ on the Y bytes
        # the bug is BEFORE GM=07 — flag it.
        rs = _sample_all(sock, f, 'rec')
        es = _sample_all(sock, f, 'emu')
        diff0 = [(k, rs[k], es[k]) for k in rs if rs[k] != es[k]]
        if diff0:
            print(f'[sync@gm07] Y-state ALREADY differs at first GM=07 '
                  f'sample (bug is upstream of gameplay):')
            for k, r, e in diff0:
                print(f'    {k}: rec=${r:02x} emu=${e:02x}')
        else:
            print('[sync@gm07] Y-state matches at first GM=07 sample.')

        # Lockstep loop. Each iteration: step 1 on both, sample, diff.
        first_diverge = None
        frame_log = []  # (frame, rec_dict, emu_dict, divergent_keys)
        for fi in range(1, MAX_FRAMES + 1):
            cmd(sock, f, 'step 1')
            cmd(sock, f, 'emu_step 1')
            rs = _sample_all(sock, f, 'rec')
            es = _sample_all(sock, f, 'emu')
            diff = [k for k in rs if rs[k] != es[k]]
            frame_log.append((fi, rs, es, diff))
            if diff and first_diverge is None:
                first_diverge = fi
                print(f'\n[FIRST DIVERGE] post-GM07 frame +{fi}:')
                for k in diff:
                    print(f'    {k}: rec=${rs[k]:02x} emu=${es[k]:02x}')
                # Print 3 frames before for context.
                print(f'  context (last 3 matching frames before):')
                for prev in frame_log[-4:-1]:
                    pf, prs, pes, _ = prev
                    print(f'    +{pf}: rec Y={prs["YPosHi"]:02x}{prs["YPosLo"]:02x} '
                          f'speed={prs["YSpeed"]:02x}  '
                          f'emu Y={pes["YPosHi"]:02x}{pes["YPosLo"]:02x} '
                          f'speed={pes["YSpeed"]:02x}')
                break
        else:
            print(f'\n[NO DIVERGE] {MAX_FRAMES} frames in lockstep, '
                  f'Y-state matches throughout. Issue B may need a '
                  f'longer probe window or the lockstep step model '
                  f'is masking the bug.')
            return

        # Now arm trace on both sides for the diverging Y byte and
        # advance one more frame to capture the writer call chain.
        # Pick the first divergent address from the SAMPLE list.
        div_addrs = [a for a, lbl in SAMPLE
                     if frame_log[-1][3] and lbl in frame_log[-1][3]]
        if not div_addrs:
            print('  (no divergent SAMPLE addr to trace?)')
            return
        first_addr = div_addrs[0]
        print(f'\n[trace] arming writer trace on $0096..$00D4 / $7D '
              f'(focus ${first_addr:04x}) for next-frame attribution...')
        cmd(sock, f, 'trace_wram_reset')
        # Cover all sample addrs for completeness.
        addr_list = ' '.join(f'{a:x}' for a, _ in SAMPLE)
        cmd(sock, f, f'trace_wram {addr_list}')
        cmd(sock, f, 'emu_wram_trace_reset')
        cmd(sock, f, f'emu_wram_trace_add {addr_list}')

        # Step one more frame on both sides to record this frame's
        # writers.
        cmd(sock, f, 'step 1'); cmd(sock, f, 'emu_step 1')

        rt = cmd(sock, f, 'get_wram_trace').get('log', [])
        et = cmd(sock, f, 'emu_get_wram_trace').get('log', [])
        print(f'\n[trace] recomp writers ({len(rt)} events):')
        for e in rt[:40]:
            print(f'   adr={e.get("adr")} val={e.get("val")} '
                  f'w={e.get("w")} func={e.get("func")} '
                  f'parent={e.get("parent")}')
        print(f'\n[trace] emu writers ({len(et)} events):')
        for e in et[:40]:
            print(f'   adr={e.get("adr")} pc={e.get("pc")} '
                  f'before={e.get("before")} after={e.get("after")} '
                  f'bank={e.get("bank_src")}')
    finally:
        try: sock.close()
        except Exception: pass
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill()
        _kill()


if __name__ == '__main__':
    main()
