"""Function-boundary-synced WRAM diff between recomp and oracle.

Both sides snapshot WRAM at every entry of HandlePlayerPhysics.
Steps both forward 1 frame, then compares the snapshots — true
sub-frame-precise sync regardless of NMI ordering.

Walks forward several frames after GM=0x07 entry, reporting the
first frame where the function-entry WRAM diverges. Flags the
diverging bytes for backwards-tracing.
"""
from __future__ import annotations
import json, pathlib, socket, subprocess, time

REPO = pathlib.Path(r'F:/Projects/SuperMarioWorldRecomp')
EXE = REPO / 'build/bin-x64-Oracle/smw.exe'

# HandlePlayerPhysics entry — main physics tick called every frame.
RECOMP_FUNC_NAME = 'HandlePlayerPhysics'
ORACLE_PC24      = 0x00D5F2


def cmd(s, f, line):
    s.sendall((line + '\n').encode())
    return json.loads(f.readline())


def hb(r):
    return bytes.fromhex(r['hex'].replace(' ', ''))


def main():
    subprocess.run(['taskkill','/F','/IM','smw.exe'],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.5)
    p = subprocess.Popen([str(EXE),'--paused'], cwd=REPO,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        s = socket.socket()
        for _ in range(50):
            try: s.connect(('127.0.0.1', 4377)); break
            except (ConnectionRefusedError, OSError): time.sleep(0.2)
        f = s.makefile('r'); f.readline()

        # Advance both sides to GM=0x07
        for _ in range(3000):
            cmd(s, f, 'step 1')
            if int(cmd(s, f, 'dump_ram 0x100 1')['hex'].replace(' ',''),16) == 7:
                break
        for _ in range(3000):
            cmd(s, f, 'emu_step 1')
            if int(cmd(s, f, 'emu_read_wram 0x100 1')['hex'].replace(' ',''),16) == 7:
                break

        # Align TrueFrame ($13/$14, 16-bit). One side may have entered
        # GM=0x07 a frame ahead; advance the lagging side until both
        # have the same TrueFrame counter.
        def r_tf(): return int(cmd(s,f,'dump_ram 0x13 2')['hex'].replace(' ',''),16)
        def o_tf(): return int(cmd(s,f,'emu_read_wram 0x13 2')['hex'].replace(' ',''),16)
        # The bytes from dump_ram are little-endian; same for emu_read_wram (which is hex blob).
        # Convert: low byte first.
        def le(s_): return int(s_[0:2],16) | (int(s_[2:4],16)<<8) if len(s_)>=4 else int(s_,16)
        def r_tfe():
            return le(cmd(s,f,'dump_ram 0x13 2')['hex'].replace(' ',''))
        def o_tfe():
            return le(cmd(s,f,'emu_read_wram 0x13 2')['hex'].replace(' ',''))
        rt, ot = r_tfe(), o_tfe()
        print(f'After GM=0x07 entry: recomp TrueFrame={rt}, oracle TrueFrame={ot}')
        # Catch up the lagging side.
        for _ in range(60):
            if rt == ot: break
            if rt < ot:
                cmd(s, f, 'step 1'); rt = r_tfe()
            else:
                cmd(s, f, 'emu_step 1'); ot = o_tfe()
        print(f'After alignment: recomp TrueFrame={rt}, oracle TrueFrame={ot}')

        # Arm the snapshot on both sides.
        r = cmd(s, f, f'func_snap_set {RECOMP_FUNC_NAME}')
        print(f'recomp func_snap_set: {r}')
        r = cmd(s, f, f'emu_func_snap_set {ORACLE_PC24:x}')
        print(f'oracle emu_func_snap_set: {r}')

        # Sync via snap counts: each side counts HandlePlayerPhysics
        # entries observed. After each dwell, ensure both sides have
        # observed the same number of calls. This gives sub-frame
        # alignment regardless of NMI ordering or per-frame call
        # multiplicity differences.
        def r_count():
            return cmd(s, f, 'func_snap_get 0 1').get('count', -1)
        def o_count():
            return cmd(s, f, 'emu_func_snap_get 0 1').get('count', -1)
        for dwell in range(1, 200):
            cmd(s, f, 'step 1')
            cmd(s, f, 'emu_step 1')
            # Catch up to equal counts.
            for _ in range(20):
                rc = r_count(); oc = o_count()
                if rc == oc: break
                if rc < oc:
                    cmd(s, f, 'step 1')
                else:
                    cmd(s, f, 'emu_step 1')

            # Read $00-$1FFF on both sides — covers DP + sprite + level state.
            # func_snap_get takes len in DECIMAL.
            rr = cmd(s, f, 'func_snap_get 0 8192')
            oo = cmd(s, f, 'emu_func_snap_get 0 8192')
            if not rr.get('ok') or not oo.get('ok'):
                # If either side hasn't fired yet, skip this frame.
                continue
            rb = hb(rr)
            ob = hb(oo)
            n = min(len(rb), len(ob), 0x2000)
            diffs = [i for i in range(n) if rb[i] != ob[i]]
            # Filter recompiler scratch ($00-$03)
            diffs = [i for i in diffs if i >= 4]
            if diffs:
                # Report first 16 diffs.
                top = diffs[:16]
                print(f'  dwell={dwell:3} '
                      f'(recomp_count={rr["count"]} oracle_count={oo["count"]}) '
                      f'{len(diffs)} diffs; first: {[hex(a) for a in top]}')
                # Show first byte's recomp/oracle values.
                first = diffs[0]
                print(f'    @0x{first:04x}: recomp=0x{rb[first]:02x} oracle=0x{ob[first]:02x}')
                # Bail if too many divergences — pursue the first dwell.
                if dwell > 5 and len(diffs) > 50:
                    break
    finally:
        s.close()
        p.terminate()
        try: p.wait(timeout=5)
        except subprocess.TimeoutExpired: p.kill()
        subprocess.run(['taskkill','/F','/IM','smw.exe'],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


if __name__ == '__main__':
    main()
