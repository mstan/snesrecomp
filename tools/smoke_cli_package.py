"""Exercise a packaged CLI with a synthetic, redistributable SNES ROM."""

from __future__ import annotations

import argparse
import pathlib
import subprocess
import tempfile


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("executable")
    args = parser.parse_args()

    executable = pathlib.Path(args.executable).resolve()
    if not executable.is_file():
        parser.error(f"CLI executable not found: {executable}")

    with tempfile.TemporaryDirectory(prefix="snesrecomp-cli-smoke-") as directory:
        root = pathlib.Path(directory)
        rom = bytearray([0xFF] * 0x8000)
        rom[0] = 0x60  # RTS at $00:8000
        rom[0x7FC0 + 0x15] = 0x20  # standard LoROM mapping byte
        rom[0x7FC0 + 0x1C:0x7FC0 + 0x20] = bytes([0xFF, 0xFF, 0, 0])
        for offset in (0x0A, 0x0E, 0x1C):
            rom[0x7FE0 + offset:0x7FE0 + offset + 2] = bytes([0x00, 0x80])
        rom_path = root / "fixture.sfc"
        rom_path.write_bytes(rom)
        output = root / "project"

        subprocess.run([
            str(executable), "build",
            "--rom", str(rom_path),
            "--output", str(output),
            "--name", "CI Fixture",
        ], check=True)
        required = (
            output / "CMakeLists.txt",
            output / "build.ps1",
            output / "config" / "bank00.cfg",
            output / "generated" / "dispatch_v2.c",
            output / "generated" / "program_manifest.json",
            output / "snesrecomp" / "runner" / "runner.cmake",
        )
        missing = [str(path) for path in required if not path.is_file()]
        if missing:
            raise RuntimeError(f"packaged CLI omitted expected output: {missing}")
    print("packaged CLI smoke test passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
