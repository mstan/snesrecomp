"""Mechanically attribute VRAM divergence via cmd_vram_write_diff.

Replaces the per-symptom probe_layer3_vram_writers.py pattern. The
recomp + oracle (snes9x) backends both ship always-on VRAM byte-write
rings; this probe walks both rings forward in lockstep through a
caller-supplied byte-address range and reports the FIRST divergent
(byte_addr, value) pair, including the recomp-side function name,
call stack, and CpuState (A/X/Y/D/DB/P/m/x) at the moment of the
write.

Per global rule "never arm-then-attach": the probe does NOT arm any
trace. It connects to a free-running Oracle build, lets it warm up
for N frames, pauses, and queries the rings backward in history.

Usage:
    python probe_vram_write_diff.py [--port PORT] [--frames N]
                                    [--lo HEX] [--hi HEX]

Default range: $A400-$B800 (the Layer-3 / HUD byte-VRAM region the
2026-04-30 corruption analysis flagged via ClearLayer3Tilemap).
"""
import argparse
import json
import socket
import sys
import time


_pending = b""


def recv_line(s):
    global _pending
    while b"\n" not in _pending:
        chunk = s.recv(1 << 18)
        if not chunk:
            break
        _pending += chunk
    nl = _pending.find(b"\n")
    if nl < 0:
        out, _pending = _pending, b""
    else:
        out, _pending = _pending[:nl], _pending[nl + 1:]
    return out.decode(errors="replace").strip()


def cmd(s, line):
    s.sendall((line + "\n").encode())
    return recv_line(s)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--port", type=int, default=4377)
    p.add_argument("--frames", type=int, default=30)
    p.add_argument("--lo", default="0xA400")
    p.add_argument("--hi", default="0xB800")
    args = p.parse_args()
    lo = int(args.lo, 16)
    hi = int(args.hi, 16)

    s = socket.create_connection(("127.0.0.1", args.port), timeout=5)
    s.settimeout(15)
    print("banner:", recv_line(s))
    print("ping:", cmd(s, "ping"))

    print(f"continue + sleep {args.frames}f...")
    print("continue:", cmd(s, "continue"))
    time.sleep(args.frames / 60.0 + 0.3)
    print("pause:", cmd(s, "pause"))

    # Walk the rings.
    diff_raw = cmd(s, f"vram_write_diff {lo:x} {hi:x}")
    try:
        d = json.loads(diff_raw)
    except json.JSONDecodeError as e:
        print("BAD JSON from vram_write_diff:", e)
        print(diff_raw[:1024])
        return 1

    if "error" in d:
        print("differ error:", d["error"])
        return 1

    print()
    print(f"=== vram_write_diff ${lo:04X}-${hi:04X} ===")
    if not d.get("diverged", False):
        print(f"NO DIVERGENCE in range "
              f"(matched {d.get('matched_pairs', 0)} pairs)")
        if d.get("recomp_exhausted") or d.get("oracle_exhausted"):
            print(f"  recomp_exhausted={d.get('recomp_exhausted')}, "
                  f"oracle_exhausted={d.get('oracle_exhausted')}")
        print()
        print("Interpretation: every (byte_addr, value) pair the recomp")
        print("emitted into this range was matched 1-to-1 by the oracle.")
        print("If gameplay still shows corruption in this region, look")
        print("for a writer that fires AFTER the captured window or via")
        print("a non-PPU path (DMA, direct g_ppu->vram poke).")
        return 0

    # Mismatch: print rich attribution.
    rec = d["recomp"]
    ora = d["oracle"]
    print(f"DIVERGED at idx={d['first_diff_idx']} "
          f"(matched {d['matched_pairs_before']} pairs first)")
    print()
    print("Recomp:")
    print(f"  byte_addr {rec['adr_byte']}  val {rec['val']}")
    print(f"  func      {rec['func']}    frame {rec['f']}")
    print(f"  A={rec['A']}  X={rec['X']}  Y={rec['Y']}  "
          f"D={rec['D']}  DB={rec['DB']}  P={rec['P']}  "
          f"m={rec['m']}  x={rec['x']}")
    print(f"  stack: {' -> '.join(rec.get('stack', [])[-8:])}")
    print()
    print("Oracle:")
    print(f"  byte_addr {ora['adr_byte']}  val {ora['val']}    "
          f"frame {ora['f']}")
    print()
    print(f"Delta: oracle wrote {ora['val']} to {ora['adr_byte']}; "
          f"recomp wrote {rec['val']} to {rec['adr_byte']}.")

    # Sanity: also call last_vram_write_to to confirm what's in the ring
    # for that byte_addr right now (post-pause).
    addr_hex = ora['adr_byte']
    addr_int = int(addr_hex, 16)
    last_raw = cmd(s, f"last_vram_write_to {addr_int:x}")
    try:
        last = json.loads(last_raw)
        if last.get("found"):
            print()
            print(f"Most-recent recomp write to {addr_hex}: "
                  f"val={last['val']} func={last['func']} "
                  f"frame={last['f']}")
        else:
            print()
            print(f"No recomp write to {addr_hex} in ring depth "
                  f"{last.get('ring_depth')}.")
    except json.JSONDecodeError:
        pass

    return 0


if __name__ == "__main__":
    sys.exit(main())
