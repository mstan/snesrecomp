#!/usr/bin/env python3
"""Small persistent client for the snesrecomp Lua TCP spike (Python stdlib)."""
import argparse
import json
from pathlib import Path
import socket
import time


class LuaClient:
    def __init__(self, host="127.0.0.1", port=4380, timeout=10):
        self.socket = socket.create_connection((host, port), timeout)
        self.reader = self.socket.makefile("rb")

    def close(self):
        self.reader.close()
        self.socket.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def command(self, command):
        data = (command + "\n").encode("ascii")
        if b"\n" in data[:-1] or len(data) >= 131072:
            raise ValueError("command contains a newline or exceeds the wire limit")
        self.socket.sendall(data)
        line = self.reader.readline(131072)
        if not line.endswith(b"\n"):
            raise ConnectionError("server disconnected or sent an oversized response")
        result = json.loads(line)
        if not result["ok"]:
            raise RuntimeError(result["error"])
        return result

    def eval(self, source):
        return self.command("eval " + source.encode("utf-8").hex())

    def run(self, source):
        return self.command("run " + source.encode("utf-8").hex())

    def step(self, frames=1, timeout=30):
        # Caller pauses first; the acknowledgement is before simulation.
        before = self.command("pause")["frame"]
        self.command(f"step {frames}")
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            result = self.command("status")
            if result["frame"] >= before + frames:
                if result["frame"] != before + frames:
                    raise RuntimeError("script resumed/advanced beyond the requested frame")
                return result
            time.sleep(0.01)
        raise TimeoutError("game did not finish stepping")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=4380)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("eval").add_argument("source")
    for name in ("load", "run"):
        commands.add_parser(name).add_argument("file", type=Path)
    commands.add_parser("step").add_argument("frames", type=int)
    for name in ("status", "pause", "resume", "stop", "reset", "ping"):
        commands.add_parser(name)
    args = parser.parse_args()
    with LuaClient(args.host, args.port) as client:
        if args.command == "eval":
            result = client.eval(args.source)
        elif args.command in ("load", "run"):
            source = args.file.read_text(encoding="utf-8")
            result = client.run(source) if args.command == "run" else client.eval(source)
        elif args.command == "step":
            result = client.step(args.frames)
        else:
            result = client.command(args.command)
        print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
