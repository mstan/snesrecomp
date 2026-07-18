"""Build the supported Rust whole-program analyzer reproducibly."""

from __future__ import annotations

import argparse
import os
import pathlib
import shutil
import subprocess
import sys


REPO = pathlib.Path(__file__).resolve().parent.parent
CRATE = REPO / "recompiler-rs"
BINARY_NAME = "snesrecomp-analyze.exe" if os.name == "nt" \
    else "snesrecomp-analyze"


def _run(command: list[str]) -> None:
    rendered = subprocess.list2cmdline(command)
    print(f"build_native_analyzer: {rendered}")
    subprocess.run(command, cwd=CRATE, check=True)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="build the snesrecomp native analyzer")
    parser.add_argument(
        "--test", action="store_true",
        help="run the Rust test suite before building")
    parser.add_argument(
        "--debug", action="store_true",
        help="build an unoptimized development binary")
    args = parser.parse_args()

    cargo = shutil.which("cargo")
    if cargo is None:
        parser.error(
            "Cargo is not installed; install Rust from https://rustup.rs/ "
            "or use a prebuilt CI/release artifact")

    common = [cargo, "--locked"]
    if args.test:
        command = common + ["test"]
        if not args.debug:
            command.append("--release")
        _run(command)

    command = common + ["build"]
    if not args.debug:
        command.append("--release")
    command += ["--bin", "snesrecomp-analyze"]
    _run(command)

    profile = "debug" if args.debug else "release"
    binary = CRATE / "target" / profile / BINARY_NAME
    if not binary.is_file():
        raise RuntimeError(f"Cargo succeeded but did not create {binary}")
    print(f"build_native_analyzer: ready: {binary}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as exc:
        print(
            f"build_native_analyzer: command failed with exit code "
            f"{exc.returncode}", file=sys.stderr)
        raise SystemExit(exc.returncode)
