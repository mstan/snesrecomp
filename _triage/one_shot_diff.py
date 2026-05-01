"""One-shot probe: step N frames, query differ, report.

Used to capture the boot-time VRAM write window before the rings wrap.
Runs synchronous step commands; multi-line step ack is handled by
draining all lines until 'stepping_complete' arrives.
"""
import argparse, json, socket, sys, time

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--port", type=int, default=4377)
    p.add_argument("--steps", type=int, default=60)
    p.add_argument("--lo", default="0xA400")
    p.add_argument("--hi", default="0xB800")
    args = p.parse_args()

    s = socket.create_connection(("127.0.0.1", args.port), timeout=8)
    s.settimeout(60)

    buf = [b""]

    def recv_line():
        while b"\n" not in buf[0]:
            c = s.recv(1 << 18)
            if not c:
                break
            buf[0] += c
        nl = buf[0].find(b"\n")
        if nl < 0:
            out, buf[0] = buf[0], b""
        else:
            out, buf[0] = buf[0][:nl], buf[0][nl + 1:]
        return out.decode(errors="replace").strip()

    # Drain banner + any boot async messages.
    print("banner:", recv_line())

    # cmd helper that drains a single line.
    def cmd(line):
        s.sendall((line + "\n").encode())
        return recv_line()

    print("frame_initial:", cmd("frame"))

    # Issue step N. Drain lines until we see "stepping_complete".
    s.sendall(f"step {args.steps}\n".encode())
    deadline = time.time() + 30
    final = None
    while time.time() < deadline:
        ln = recv_line()
        if not ln:
            time.sleep(0.05)
            continue
        try:
            j = json.loads(ln)
        except json.JSONDecodeError:
            print("non-json:", ln)
            continue
        if "stepping_complete" in j or j.get("ok") is True:
            final = j
            break
        # Progress / ack line, keep draining.
    print("step result:", final)
    print("frame_after:", cmd("frame"))

    # Quick sanity: how many entries in each ring?
    print("\n--- rings ---")
    print("recomp ring sample (last 3):")
    raw = cmd("get_vram_trace nostack")
    try:
        d = json.loads(raw)
        log = d.get("log", [])
        print(f"  entries={d.get('entries')} returned={len(log)}")
        for e in log[-3:]:
            print(f"    {e}")
    except json.JSONDecodeError:
        print("BAD JSON; first 400:", raw[:400])

    print("oracle ring sample (last 3):")
    raw = cmd("get_oracle_vram_trace")
    try:
        d = json.loads(raw)
        log = d.get("log", [])
        print(f"  entries={d.get('entries')} returned={len(log)}")
        for e in log[-3:]:
            print(f"    {e}")
    except json.JSONDecodeError:
        print("BAD JSON; first 400:", raw[:400])

    # The differ.
    lo = int(args.lo, 16)
    hi = int(args.hi, 16)
    diff_raw = cmd(f"vram_write_diff {lo:x} {hi:x}")
    print(f"\nvram_write_diff ${lo:04X}-${hi:04X}:")
    print(diff_raw[:2048])

    return 0


if __name__ == "__main__":
    sys.exit(main())
