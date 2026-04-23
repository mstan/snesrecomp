"""Verify the find_first_divergence TCP command.

Scope: checks the command's shape on the Oracle build. It should report
either (a) match=true when recomp and oracle agree over a range, or
(b) match=false with a first_diff + context window when they disagree.

We drive divergence deterministically by writing a known value to recomp's
WRAM via `write_ram` while the oracle side keeps its own copy. Then
`find_first_divergence` should pinpoint that address as the first diff.

Run against a live Oracle-build exe (build/bin-x64-Oracle/smw.exe) with
the debug server on port 4377.
"""
from __future__ import annotations
import json
import socket
import sys
import time


def _cmd(sock: socket.socket, f, line: str) -> dict:
    sock.sendall((line + '\n').encode())
    return json.loads(f.readline())


def main(host: str = '127.0.0.1', port: int = 4377) -> int:
    sock = socket.socket()
    sock.connect((host, port))
    f = sock.makefile('r')
    banner = f.readline()
    if 'connected' not in banner:
        print(f'unexpected banner: {banner!r}')
        return 1

    failures = []
    time.sleep(0.5)

    # 0. Confirm backend is active.
    r = _cmd(sock, f, 'emu_list')
    if not r.get('ok') or not r.get('active'):
        failures.append(f"emu_list: no active backend — {r}")
        for msg in failures:
            print(f'FAIL: {msg}')
        return 1

    # 1. Unknown subsystem returns structured error, not a crash.
    r = _cmd(sock, f, 'find_first_divergence vram 0 0x100')
    if r.get('ok') is not False or 'supported' not in r:
        failures.append(
            f"find_first_divergence with unsupported subsystem: expected "
            f"structured error with 'supported' list, got {r}"
        )

    # 2. Scan full WRAM. Recomp and oracle have been running lock-step
    #    since init, so they should mostly agree — but they may not be
    #    bit-identical (this is the very bug we're investigating). We
    #    only check that the command succeeds and returns a well-shaped
    #    response.
    r = _cmd(sock, f, 'find_first_divergence wram 0 0x1FFFF 8')
    if r.get('ok') is not True:
        failures.append(f"find_first_divergence full-range: ok=False — {r}")
    elif r.get('match') is True:
        # They happen to match — print info but accept.
        print(f"INFO: recomp and oracle WRAM already match ({r.get('bytes_scanned')} bytes).")
    else:
        need = {'first_diff', 'recomp', 'oracle', 'diff_count', 'context'}
        missing = need - set(r.keys())
        if missing:
            failures.append(
                f"find_first_divergence divergent response missing fields: "
                f"{sorted(missing)}"
            )
        else:
            ctx = r.get('context') or []
            if not isinstance(ctx, list):
                failures.append(f"context is not a list: {type(ctx)}")
            elif not ctx:
                failures.append(f"context window is empty")
            else:
                # At least one context entry must have diff=true and match
                # the first_diff address.
                first = int(r['first_diff'], 16)
                diff_entries = [e for e in ctx if e.get('diff') is True]
                if not diff_entries:
                    failures.append(
                        f"no diff=true entries in context window: {ctx[:3]}"
                    )
                elif not any(int(e['adr'], 16) == first for e in diff_entries):
                    failures.append(
                        f"first_diff {r['first_diff']} not present in "
                        f"context window diffs"
                    )

    # 3. Inject a deterministic divergence: pause the game, write a byte into
    #    recomp's WRAM that we know differs from the oracle's current state,
    #    then verify the finder reports that address. Pausing is essential —
    #    without it the game advances between write_ram and find, and the
    #    injected byte is overwritten by normal gameplay.
    _cmd(sock, f, 'pause')
    probe_addr = 0x1FFFE  # bank 7F top — unlikely to be rewritten while paused

    r = _cmd(sock, f, f'emu_read_wram 0x{probe_addr:05x} 1')
    oracle_val = int(r['hex'], 16) if r.get('ok') else 0
    r = _cmd(sock, f, f'dump_ram 0x{probe_addr:x} 1')
    recomp_val = int(r['hex'], 16)

    # Choose an injection that differs from BOTH sides.
    injected_val = (oracle_val ^ 0x5A) & 0xFF
    if injected_val == recomp_val:
        injected_val ^= 0x01
    _cmd(sock, f, f'write_ram 0x{probe_addr:x} {injected_val:02x}')

    # Confirm the write landed before running the finder.
    r = _cmd(sock, f, f'dump_ram 0x{probe_addr:x} 1')
    recomp_val_after = int(r['hex'], 16)
    if recomp_val_after != injected_val:
        failures.append(
            f"write_ram injection did not persist: wrote 0x{injected_val:02x}, "
            f"read back 0x{recomp_val_after:02x}"
        )

    # Narrow-range scan at the probe address only.
    r = _cmd(sock, f, f'find_first_divergence wram 0x{probe_addr:x} 0x{probe_addr:x} 0')
    if r.get('ok') is not True:
        failures.append(f"narrow-range find: ok=False — {r}")
    elif r.get('match') is True:
        failures.append(
            f"narrow-range find after write_ram injection: expected diff "
            f"at 0x{probe_addr:x}, got match=true (oracle=0x{oracle_val:02x} "
            f"injected=0x{injected_val:02x})"
        )
    else:
        got = int(r.get('first_diff', '0x0'), 16)
        if got != probe_addr:
            failures.append(
                f"narrow-range find: expected first_diff=0x{probe_addr:x}, "
                f"got 0x{got:x}"
            )
        if int(r.get('recomp', '0x0'), 16) != injected_val:
            failures.append(
                f"narrow-range find: expected recomp=0x{injected_val:02x}, "
                f"got {r.get('recomp')}"
            )

    _cmd(sock, f, 'continue')

    sock.close()

    if failures:
        print('FAIL:')
        for msg in failures:
            print(f'  {msg}')
        return 1
    print('OK: find_first_divergence reports correctness, shape, and injected diffs.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
