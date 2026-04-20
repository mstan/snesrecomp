"""Diagnostic probe (NOT a committed test): launch oracle (pure
interpreter, --theirs) and recomp, advance both to the same attract-mode
frame, dump VRAM, diff.

Goal: attribute the 'missing ground tiles' visual regression to specific
VRAM regions / word addresses that the recomp fails to write.

Assumes both binaries are at the same ROM + config and have determinstic
attract-mode playback from boot. If that assumption is wrong, this probe
exposes it (first frame of divergence) rather than hiding it.
"""
import json
import os
import pathlib
import socket
import subprocess
import sys
import time

THIS_DIR = pathlib.Path(__file__).parent
sys.path.insert(0, str(THIS_DIR))

from harness import RECOMP_EXE, ORACLE_EXE, RECOMP_PORT, ORACLE_PORT  # noqa: E402
from harness import DebugClient  # noqa: E402


def _kill_existing():
    subprocess.run(['taskkill', '/F', '/IM', 'smw.exe'],
                   capture_output=True, check=False)


def _ports_ready():
    for port in (RECOMP_PORT, ORACLE_PORT):
        try:
            s = socket.create_connection(('127.0.0.1', port), timeout=0.3)
            s.close()
        except (OSError, ConnectionRefusedError):
            return False
    return True


def launch_both_interp():
    """Launch recomp paused; launch oracle paused with --theirs so it
    runs pure interpreter (RM_THEIRS) frame-by-frame, not RM_BOTH.
    """
    _kill_existing()
    time.sleep(0.5)
    subprocess.Popen(
        [str(RECOMP_EXE), '--paused'],
        cwd=str(RECOMP_EXE.parent.parent.parent),
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    subprocess.Popen(
        [str(ORACLE_EXE), '--paused', '--theirs'],
        cwd=str(ORACLE_EXE.parent.parent.parent),
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    deadline = time.time() + 15
    while time.time() < deadline:
        if _ports_ready():
            time.sleep(0.3)
            return
        time.sleep(0.2)
    raise RuntimeError('timeout waiting for ports')


def run_to_frame(client, target):
    r = client.cmd(f'run_to_frame {target}')
    if not r.get('ok'):
        raise RuntimeError(f'run_to_frame {target}: {r}')
    # Poll frame counter until we hit the target. cmd_frame returns current.
    deadline = time.time() + 60
    while time.time() < deadline:
        f = client.cmd('frame')
        cur = f.get('frame', 0)
        if cur >= target:
            return cur
        time.sleep(0.2)
    raise RuntimeError(f'timeout running to frame {target}, stuck at {cur}')


def dump_vram(client):
    r = client.cmd('dump_vram 0 65536')  # full 64KB (addr hex, len decimal)
    hex_str = r.get('hex', '')
    return bytes.fromhex(hex_str)


def diff_vram(rv, ov):
    """Return list of (word_addr, recomp_word, oracle_word) diffs.
    VRAM is 32K words; iterate word-by-word."""
    diffs = []
    for word_addr in range(0x8000):
        r_lo = rv[word_addr * 2]
        r_hi = rv[word_addr * 2 + 1]
        o_lo = ov[word_addr * 2]
        o_hi = ov[word_addr * 2 + 1]
        if r_lo != o_lo or r_hi != o_hi:
            diffs.append((word_addr, (r_hi << 8) | r_lo, (o_hi << 8) | o_lo))
    return diffs


def main():
    target_frame = int(sys.argv[1]) if len(sys.argv) > 1 else 100
    launch_both_interp()
    r = DebugClient(RECOMP_PORT)
    o = DebugClient(ORACLE_PORT)
    try:
        # _ports_ready opened/closed probe sockets, which auto-unpaused
        # the runners (the server treats disconnect as "resume"). Re-pause
        # explicitly now that we're on the persistent connection.
        r.cmd('pause')
        o.cmd('pause')
        # Reset the frame counter baseline
        base_r = r.cmd('frame').get('frame', 0)
        base_o = o.cmd('frame').get('frame', 0)
        print(f'[init] baseline recomp frame={base_r}, oracle frame={base_o}')
        # Convert target to absolute (relative from baseline)
        target_abs_r = base_r + target_frame
        target_abs_o = base_o + target_frame
        # Issue run commands back-to-back
        print(f'[run] stepping both by exactly {target_frame} frames')
        # Use `step N` — it auto-pauses exactly at start+N frames,
        # avoiding run_to_frame's 1-2 frame overshoot.
        r.cmd(f'step {target_frame}')
        o.cmd(f'step {target_frame}')
        deadline = time.time() + 60
        while time.time() < deadline:
            rf = r.cmd('frame').get('frame', 0)
            of = o.cmd('frame').get('frame', 0)
            if rf >= target_abs_r and of >= target_abs_o:
                break
            time.sleep(0.1)
        print(f'[run] recomp at frame={rf}, oracle at frame={of}')
        print(f'[dump] reading VRAM from both (64KB each)')
        rv = dump_vram(r)
        ov = dump_vram(o)
        print(f'[dump] recomp={len(rv)} bytes, oracle={len(ov)} bytes')
        diffs = diff_vram(rv, ov)
        print(f'[diff] {len(diffs)} word-level differences')
        # Pattern analysis
        hi_zero_lo_match = sum(
            1 for _, rw, ow in diffs
            if (rw & 0x00FF) == (ow & 0x00FF) and (rw & 0xFF00) == 0
        )
        lo_zero_hi_match = sum(
            1 for _, rw, ow in diffs
            if (rw & 0xFF00) == (ow & 0xFF00) and (rw & 0x00FF) == 0
        )
        both_diff = len(diffs) - hi_zero_lo_match - lo_zero_hi_match
        print(f'[pattern] {hi_zero_lo_match} words: recomp has correct low byte, high byte zero (recomp-missing-hi)')
        print(f'[pattern] {lo_zero_hi_match} words: recomp has correct high byte, low byte zero (recomp-missing-lo)')
        print(f'[pattern] {both_diff} words: both bytes differ')
        # Scope: which VRAM $1000-word buckets have the most diffs?
        buckets_4k = {}
        for w_addr, _, _ in diffs:
            key = w_addr & 0xF000
            buckets_4k[key] = buckets_4k.get(key, 0) + 1
        print('[scope] diffs per VRAM 4K-word bucket:')
        for k in sorted(buckets_4k):
            print(f'    $V{k:04x}-$V{k+0xfff:04x}: {buckets_4k[k]} diffs')
        if not diffs:
            print('  (no divergence — recomp and interpreter match)')
            return
        # Bucket into contiguous regions for readability
        buckets = []
        cur = [diffs[0]]
        for d in diffs[1:]:
            if d[0] == cur[-1][0] + 1:
                cur.append(d)
            else:
                buckets.append(cur)
                cur = [d]
        buckets.append(cur)
        print(f'[diff] {len(buckets)} contiguous divergent regions:')
        for i, bucket in enumerate(buckets[:20]):
            start = bucket[0][0]
            end = bucket[-1][0]
            print(f'  region {i}: $V{start:04x}..$V{end:04x} ({len(bucket)} words)')
            # show first 3 words
            for w_addr, rw, ow in bucket[:3]:
                print(f'    $V{w_addr:04x}: recomp=0x{rw:04x} oracle=0x{ow:04x}')
        if len(buckets) > 20:
            print(f'  ... ({len(buckets) - 20} more regions)')
    finally:
        r.close()
        o.close()
        _kill_existing()


if __name__ == '__main__':
    main()
