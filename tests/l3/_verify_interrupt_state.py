"""Verify that NMI/IRQ state is observable via both the live
`get_interrupt_state` command and the historical `get_frame_extended`
snapshot.

Prior to Phase 1c, interrupt state was entirely invisible to the debugger:
the Cpu struct's interpreter-era nmiWanted/irqWanted were ripped as
write-only-never-read, and the Snes struct's live inNmi/inIrq/inVblank
fields were never wired up to TCP. Any bug involving NMI timing,
vblank sequencing, or IRQ scheduling was undiagnosable.

Run against a live recomp exe with the debug server on port 4377.
Exits 0 on success, 1 on failure.
"""
from __future__ import annotations
import json
import socket
import sys
import time


REQUIRED_FIELDS = {
    'inNmi', 'inIrq', 'inVblank',
    'nmiEnabled', 'hIrqEnabled', 'vIrqEnabled',
    'autoJoyRead', 'hPos', 'vPos', 'hTimer', 'vTimer',
    'autoJoyTimer',
}


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
    time.sleep(0.5)  # let a few frames run

    # 1. Live get_interrupt_state returns all required fields.
    r = _cmd(sock, f, 'get_interrupt_state')
    if 'error' in r:
        failures.append(f"get_interrupt_state error: {r['error']}")
    else:
        missing = REQUIRED_FIELDS - set(r.keys())
        if missing:
            failures.append(
                f"get_interrupt_state missing fields: {sorted(missing)}"
            )
        # nmiEnabled should be true on a game that's past boot (SMW enables
        # NMI early). If not, flag it — either the capture is wrong or the
        # runtime state is nonsensical.
        if r.get('nmiEnabled') is not True:
            failures.append(
                f"get_interrupt_state nmiEnabled={r.get('nmiEnabled')} — "
                f"SMW should have NMI enabled past boot"
            )

    # 2. Historical get_frame_extended includes "irq" block.
    pong = _cmd(sock, f, 'ping')
    current_frame = pong.get('frame', 0)
    found_frame = None
    for back in range(2, 20):
        target = current_frame - back
        if target < 0:
            break
        r = _cmd(sock, f, f'get_frame_extended {target}')
        if 'error' not in r:
            found_frame = target
            break
    if found_frame is None:
        failures.append('get_frame_extended: no frame found in history')
    else:
        irq = r.get('irq')
        if not irq:
            failures.append(
                f"get_frame_extended frame {found_frame}: missing 'irq' block"
            )
        else:
            missing = REQUIRED_FIELDS - set(irq.keys())
            if missing:
                failures.append(
                    f"get_frame_extended frame {found_frame} irq missing: "
                    f"{sorted(missing)}"
                )
            # vPos should be a valid scanline number [0, 262] on a live game.
            vpos = irq.get('vPos')
            if not isinstance(vpos, int) or not (0 <= vpos <= 262):
                failures.append(
                    f"get_frame_extended frame {found_frame} irq.vPos={vpos} "
                    f"is outside [0, 262]"
                )

    sock.close()

    if failures:
        print('FAIL:')
        for msg in failures:
            print(f'  {msg}')
        return 1
    print('OK: Interrupt/timing state exposed in both live and historical endpoints.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
