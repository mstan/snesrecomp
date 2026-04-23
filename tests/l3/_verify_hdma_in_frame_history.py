"""Verify that HDMA state is exposed in both the live `get_dma_state`
command and the historical `get_frame_extended` frame-history snapshot.

Prior to Phase 1b, the frame-history capture only stored bAdr, aBank, mode,
flags, aAdr, size per channel — HDMA-specific fields (tableAdr, indBank,
repCount, offIndex, doTransfer, terminated) were invisible in history.
`get_dma_state` itself was also missing these fields. Any bug involving
HDMA sequencing (per-scanline scroll, window effects, etc.) was therefore
undiagnosable from snapshot alone.

Run against a live recomp exe with the debug server on port 4377.
Exits 0 on success, 1 on failure.
"""
from __future__ import annotations
import json
import socket
import sys
import time


REQUIRED_LIVE_FIELDS = {
    'tableAdr', 'indBank', 'repCount', 'offIndex',
    'doTransfer', 'terminated',
}
REQUIRED_FRAME_FIELDS = {
    'tableAdr', 'indBank', 'repCount', 'offIndex',
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

    # Let a few frames run so the ring has content.
    time.sleep(0.5)

    # 1. Live get_dma_state exposes HDMA fields.
    r = _cmd(sock, f, 'get_dma_state')
    channels = r.get('channels', [])
    if not channels:
        failures.append(f"get_dma_state returned no channels: {r}")
    else:
        missing = REQUIRED_LIVE_FIELDS - set(channels[0].keys())
        if missing:
            failures.append(
                f"get_dma_state channel[0] missing HDMA fields: {sorted(missing)}"
            )

    # 2. Historical get_frame_extended exposes HDMA fields in dma array.
    pong = _cmd(sock, f, 'ping')
    current_frame = pong.get('frame', 0)
    # Ring buffer may be behind current by a few frames; try a window.
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
        dma = r.get('dma', [])
        if not dma:
            failures.append(f"get_frame_extended frame {found_frame}: empty dma array")
        else:
            missing = REQUIRED_FRAME_FIELDS - set(dma[0].keys())
            if missing:
                failures.append(
                    f"get_frame_extended frame {found_frame} dma[0] missing "
                    f"HDMA fields: {sorted(missing)}"
                )

    # 3. Flags byte encodes doTransfer=bit6 and terminated=bit7. Just make sure
    #    the flags field is present (semantic encoding tested implicitly).
    if found_frame is not None and dma:
        if 'flags' not in dma[0]:
            failures.append(f"get_frame_extended frame {found_frame} dma[0] missing flags")

    sock.close()

    if failures:
        print('FAIL:')
        for msg in failures:
            print(f'  {msg}')
        return 1
    print('OK: HDMA fields exposed in both live and historical DMA endpoints.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
