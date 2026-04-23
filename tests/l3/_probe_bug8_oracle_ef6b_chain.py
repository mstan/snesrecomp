"""Bug #8 — capture oracle's per-instruction PC trace across oracle
frames 285-300 (the window containing the $72-clearing STZ at $EF6B).
Filter to PCs in the EE/EF range to see the exact JSR/BRA chain that
leads oracle into CODE_00EF60."""
from __future__ import annotations
import json, pathlib, socket, subprocess, sys, time

REPO = pathlib.Path(r'F:/Projects/SuperMarioWorldRecomp')
EXE = REPO / 'build/bin-x64-Oracle/smw.exe'


def cmd(sock, f, line):
    sock.sendall((line + '\n').encode())
    return json.loads(f.readline())


def step1(sock, f):
    b = cmd(sock, f, 'frame').get('frame', 0)
    cmd(sock, f, 'step 1')
    deadline = time.time() + 5
    while time.time() < deadline:
        if cmd(sock, f, 'frame').get('frame', 0) > b: return b + 1
        time.sleep(0.01)


def e_byte(sock, f, addr):
    r = cmd(sock, f, f'emu_read_wram 0x{addr:x} 1')
    return int(r['hex'].replace(' ', ''), 16) if r.get('ok') else -1


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
        f = sock.makefile('r')
        f.readline()

        # Quickly step recomp a long time so we're past recomp's f201
        # (and not spending time on the recomp main loop).
        # Actually — the main-loop step 1 advances both recomp and oracle.
        # We need oracle around frame 290. That means step 290 frames of
        # the main loop (which advances oracle to ~86, since oracle lags
        # ~204). Then use emu_step to push oracle to ~285.
        # Simpler: just use emu_step from the start.
        # But oracle is ticked inside recomp's main loop too, so calling
        # emu_step without stepping recomp works fine.
        # Push oracle to frame 280 via emu_step alone.
        for _ in range(280):
            cmd(sock, f, 'emu_step 1')

        # Arm insn trace, then step oracle 30 more frames to cover the
        # $72 clear event.
        cmd(sock, f, 'emu_insn_trace_reset')
        cmd(sock, f, 'emu_insn_trace_on')
        for _ in range(30):
            cmd(sock, f, 'emu_step 1')
        cmd(sock, f, 'emu_insn_trace_off')

        r = cmd(sock, f, 'emu_insn_trace_count')
        total = r.get('count', 0)
        print(f'captured {total} instructions across oracle frames 280-310')

        # First: show 30 instructions BEFORE entering EE region to see caller chain.
        # Find first entry in EE/EF range:
        first_ef_i = None
        from_idx = 0
        while True:
            r = cmd(sock, f, f'emu_get_insn_trace from={from_idx} limit=4096 pc_lo=0xee00 pc_hi=0xefff')
            log = r.get('log', [])
            if not log: break
            first_ef_i = int(log[0]['i'])
            break
        if first_ef_i is not None:
            pre_from = max(0, first_ef_i - 80)
            r = cmd(sock, f, f'emu_get_insn_trace from={pre_from} limit=90')
            print('=== 90 instructions around first EE region entry ===')
            for e in r.get('log', []):
                print(f'  i={e["i"]:7} f{e["f"]} pc={e["pc"]} op={e["op"]} '
                      f'a={e.get("a","?")} x={e.get("x","?")} y={e.get("y","?")}')

        # Pull PCs in $00EE00 - $00F000 range.
        print('\n=== PCs in $00EE00 - $00EFFF ===')
        from_idx = 0
        found = 0
        window_around_ef6b = []
        while True:
            r = cmd(sock, f, f'emu_get_insn_trace from={from_idx} limit=4096 pc_lo=0xee00 pc_hi=0xefff')
            log = r.get('log', [])
            if not log: break
            for e in log:
                pc = int(e['pc'], 16)
                if 0xee00 <= pc <= 0xefff:
                    window_around_ef6b.append((e['i'], e['f'], e['pc'], e['op'],
                                                e['a'], e['x'], e['y']))
                    found += 1
            from_idx = int(log[-1]['i']) + 1
            if from_idx >= total: break

        if not window_around_ef6b:
            print('  NO instructions executed in EE/EF region! Oracle also skipped.')
            # Check PCs in F9 region (CODE_00F94E / F9A8 / F992)
            print('\n=== PCs in $00F900 - $00F9FF (F94E / F9A8 / F992 region) ===')
            from_idx = 0
            found = 0
            while True:
                r = cmd(sock, f, f'emu_get_insn_trace from={from_idx} limit=4096 pc_lo=0xf900 pc_hi=0xf9ff')
                log = r.get('log', [])
                if not log: break
                for e in log[:50]:
                    print(f'  i={e["i"]:7} f{e["f"]} pc={e["pc"]} op={e["op"]} a={e["a"]} x={e["x"]} y={e["y"]}')
                found += len(log)
                from_idx = int(log[-1]['i']) + 1
                if from_idx >= total or found > 50: break
            return 0

        # Print the context around PC=0x00ef6d (the $72 clear).
        # Find the first insn with pc between $ef60 and $ef6f, then emit
        # 20 before and 20 after.
        target_idx = None
        for i, entry in enumerate(window_around_ef6b):
            pc = int(entry[2], 16)
            if 0xef60 <= pc <= 0xef6f:
                target_idx = i
                break
        print(f'\n=== context around $EF6B (entries in EE/EF range) ===')
        lo = max(0, (target_idx or 0) - 20)
        hi = min(len(window_around_ef6b), (target_idx or 0) + 20)
        for entry in window_around_ef6b[lo:hi]:
            print(f'  i={entry[0]:7} f{entry[1]} pc={entry[2]} op={entry[3]} '
                  f'a={entry[4]} x={entry[5]} y={entry[6]}')
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
