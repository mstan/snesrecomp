"""Verify that read_ram and dump_ram return the full requested length,
not a silently-clamped subset.

Prior to the Phase-1a fix, read_ram silently clamped len > 1024 to 1024,
which caused any probe requesting a larger range to compare only the first
1 KB. dump_ram capped at 64 KB (still less than full WRAM at 128 KB).
Both now cap at 128 KB = full WRAM.

Run against a live recomp exe with the debug server listening on port 4377
(the recomp default). Exits 0 on success, 1 on failure.
"""
from __future__ import annotations
import json
import socket
import sys


def _cmd(sock: socket.socket, f, line: str) -> dict:
    sock.sendall((line + '\n').encode())
    return json.loads(f.readline())


def _parse_read_ram_hex(hex_str: str) -> bytes:
    # read_ram returns "aa bb cc ..." (space-separated).
    if not hex_str:
        return b''
    return bytes(int(x, 16) for x in hex_str.split())


def _parse_dump_ram_hex(hex_str: str) -> bytes:
    # dump_ram returns "aabbcc..." (tight).
    return bytes.fromhex(hex_str)


def main(host: str = '127.0.0.1', port: int = 4377) -> int:
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((host, port))
    f = sock.makefile('r')
    # Discard the connection banner ({"connected":true,"frame":N}).
    banner = f.readline()
    if 'connected' not in banner:
        print(f'unexpected banner: {banner!r}')
        return 1

    failures = []

    # read_ram at 4 KB — was previously silently clamped to 1 KB.
    r = _cmd(sock, f, 'read_ram 0 4096')
    hex_str = r.get('hex', '')
    got = _parse_read_ram_hex(hex_str)
    if r.get('len') != 4096:
        failures.append(f"read_ram 0 4096: response len={r.get('len')}, want 4096")
    if len(got) != 4096:
        failures.append(f"read_ram 0 4096: decoded {len(got)} bytes, want 4096")

    # read_ram at 32 KB — well beyond the old 1 KB clamp.
    r = _cmd(sock, f, 'read_ram 0 32768')
    got = _parse_read_ram_hex(r.get('hex', ''))
    if r.get('len') != 32768 or len(got) != 32768:
        failures.append(
            f"read_ram 0 32768: response len={r.get('len')} decoded={len(got)}, want 32768"
        )

    # read_ram at full WRAM — 128 KB.
    r = _cmd(sock, f, 'read_ram 0 131072')
    got = _parse_read_ram_hex(r.get('hex', ''))
    if r.get('len') != 131072 or len(got) != 131072:
        failures.append(
            f"read_ram 0 131072: response len={r.get('len')} decoded={len(got)}, want 131072"
        )

    # dump_ram at 128 KB — was previously capped at 64 KB.
    r = _cmd(sock, f, 'dump_ram 0 131072')
    got = _parse_dump_ram_hex(r.get('hex', ''))
    if r.get('len') != 131072 or len(got) != 131072:
        failures.append(
            f"dump_ram 0 131072: response len={r.get('len')} decoded={len(got)}, want 131072"
        )

    # Consistency: read_ram and dump_ram over the same range must return the
    # same bytes. (Different formats, same content.)
    r1 = _cmd(sock, f, 'read_ram 0x100 512')
    b1 = _parse_read_ram_hex(r1.get('hex', ''))
    r2 = _cmd(sock, f, 'dump_ram 0x100 512')
    b2 = _parse_dump_ram_hex(r2.get('hex', ''))
    if b1 != b2:
        failures.append(
            f"read_ram and dump_ram disagree over $100..$300: "
            f"len read={len(b1)} dump={len(b2)}, first diff at byte "
            f"{next((i for i in range(min(len(b1), len(b2))) if b1[i] != b2[i]), 'none')}"
        )

    sock.close()

    if failures:
        print('FAIL:')
        for msg in failures:
            print(f'  {msg}')
        return 1
    print('OK: read_ram and dump_ram return full requested length up to 128 KB.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
