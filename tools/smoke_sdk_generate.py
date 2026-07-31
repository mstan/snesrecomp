"""Smoke-test headless ``generate`` / ``verify-rom`` without a real game ROM."""

from __future__ import annotations

import json
import pathlib
import subprocess
import sys
import tempfile
import zlib


ROOT = pathlib.Path(__file__).resolve().parent.parent
CLI = ROOT / "snesrecomp_cli.py"


def write_fixture_rom(path: pathlib.Path) -> bytes:
    rom = bytearray([0xFF] * 0x8000)
    rom[0] = 0x60  # RTS at $00:8000
    rom[0x7FC0 + 0x15] = 0x20  # standard LoROM mapping byte
    rom[0x7FC0 + 0x1C:0x7FC0 + 0x20] = bytes([0xFF, 0xFF, 0, 0])
    for offset in (0x0A, 0x0E, 0x1C):
        rom[0x7FE0 + offset:0x7FE0 + offset + 2] = bytes([0x00, 0x80])
    path.write_bytes(rom)
    return bytes(rom)


def run_cli(args: list[str]) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, str(CLI), *args],
        cwd=str(ROOT),
        text=True,
        capture_output=True,
        check=False,
    )


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="snesrecomp-sdk-smoke-") as directory:
        root = pathlib.Path(directory)
        rom_path = root / "fixture.sfc"
        raw = write_fixture_rom(rom_path)
        crc32 = f"{zlib.crc32(raw) & 0xFFFFFFFF:08x}"

        verify = run_cli([
            "verify-rom",
            "--rom", str(rom_path),
            "--expected-crc32", crc32,
            "--json-progress",
        ])
        if verify.returncode != 0:
            raise RuntimeError(
                f"verify-rom failed: rc={verify.returncode}\n"
                f"stdout:\n{verify.stdout}\nstderr:\n{verify.stderr}"
            )
        events = [
            json.loads(line) for line in verify.stdout.splitlines() if line.strip()
        ]
        if not any(event.get("event") == "result" and event.get("ok") for event in events):
            raise RuntimeError(f"verify-rom missing result event: {events}")

        bad = run_cli([
            "verify-rom",
            "--rom", str(rom_path),
            "--expected-crc32", "deadbeef",
        ])
        if bad.returncode != 3:
            raise RuntimeError(
                f"verify-rom should exit 3 on mismatch, got {bad.returncode}\n"
                f"stderr:\n{bad.stderr}"
            )

        cfg_dir = root / "config"
        out_dir = root / "generated"
        funcs_h = cfg_dir / "funcs.h"
        cfg_dir.mkdir(parents=True, exist_ok=True)
        (cfg_dir / "bank00.cfg").write_text(
            "bank = 0\nauto_vectors\n", encoding="utf-8")

        generate = run_cli([
            "generate",
            "--rom", str(rom_path),
            "--cfg-dir", str(cfg_dir),
            "--out-dir", str(out_dir),
            "--funcs-h", str(funcs_h),
            "--no-host-root-scan",
            "--analysis-backend", "python",
            "--expected-crc32", crc32,
            "--json-progress",
        ])
        if generate.returncode != 0:
            raise RuntimeError(
                f"generate failed: rc={generate.returncode}\n"
                f"stdout:\n{generate.stdout}\nstderr:\n{generate.stderr}"
            )
        required = (
            out_dir / "dispatch_v2.c",
            out_dir / "program_manifest.json",
            funcs_h,
        )
        missing = [str(path) for path in required if not path.is_file()]
        if missing:
            raise RuntimeError(f"generate omitted required outputs: {missing}")

        events = []
        for line in generate.stdout.splitlines():
            if not line.strip():
                continue
            try:
                events.append(json.loads(line))
            except json.JSONDecodeError as exc:
                raise RuntimeError(
                    f"generate stdout is not clean JSONL: {line!r} ({exc})\n"
                    f"full stdout:\n{generate.stdout}\nstderr:\n{generate.stderr}"
                ) from exc
        phases = [event.get("phase") for event in events if event.get("event") == "phase"]
        for required_phase in ("verify", "emit", "sync_funcs_h", "done"):
            if required_phase not in phases:
                raise RuntimeError(
                    f"generate missing phase {required_phase!r}: {phases}"
                )
        if not any(event.get("event") == "result" and event.get("ok") for event in events):
            raise RuntimeError(f"generate missing result event: {events}")

    print("sdk generate smoke test passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
