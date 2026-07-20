"""Self-contained ROM-to-source front end for snesrecomp."""

from __future__ import annotations

import argparse
import hashlib
import os
import pathlib
import re
import shutil
import sys


def resource_root() -> pathlib.Path:
    frozen = getattr(sys, "_MEIPASS", None)
    return pathlib.Path(frozen).resolve() if frozen else pathlib.Path(__file__).resolve().parent


ROOT = resource_root()
os.environ["SNESRECOMP_ROOT"] = str(ROOT)
for path in (ROOT, ROOT / "recompiler", ROOT / "tools"):
    value = str(path)
    if value not in sys.path:
        sys.path.insert(0, value)

from snes65816 import detect_rom_mapping, load_rom  # noqa: E402
from tools import v2_emit  # noqa: E402


def safe_name(value: str) -> str:
    cleaned = re.sub(r"[^A-Za-z0-9]+", "_", value).strip("_")
    if not cleaned:
        return "SNESGameRecomp"
    if cleaned[0].isdigit():
        cleaned = "Game_" + cleaned
    return cleaned + "Recomp"


def write_text(path: pathlib.Path, contents: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(contents, encoding="utf-8", newline="\n")


def run_tool(tool, arguments: list[str]) -> int:
    original = sys.argv
    try:
        sys.argv = [tool.__file__, *arguments]
        try:
            result = tool.main()
        except SystemExit as exc:
            result = exc.code
        return int(result or 0)
    finally:
        sys.argv = original


def build_project(args: argparse.Namespace) -> int:
    rom_path = pathlib.Path(args.rom).expanduser().resolve()
    output = pathlib.Path(args.output).expanduser().resolve()
    if not rom_path.is_file():
        raise ValueError(f"ROM not found: {rom_path}")
    if rom_path.suffix.lower() not in (".sfc", ".smc"):
        raise ValueError("ROM must be an .sfc or .smc file")
    raw = rom_path.read_bytes()
    if len(raw) < 32 * 1024 or len(raw) > 16 * 1024 * 1024:
        raise ValueError("ROM size is outside the supported 32 KiB to 16 MiB range")
    if len(raw) % 1024 not in (0, 512):
        raise ValueError("ROM size is not a standard SNES image size")
    if output.exists():
        if not output.is_dir():
            raise ValueError(f"output path is not a directory: {output}")
        if any(output.iterdir()):
            raise ValueError(f"output directory is not empty: {output}")

    title = args.name or rom_path.stem
    project_name = safe_name(title)
    normalized_rom = load_rom(str(rom_path))
    mapping = detect_rom_mapping(normalized_rom)

    config_dir = output / "config"
    generated_dir = output / "generated"
    config_dir.mkdir(parents=True, exist_ok=True)
    write_text(config_dir / "bank00.cfg", "bank = 0\nauto_vectors\n")
    write_text(config_dir / "funcs.h", """/* Starter declarations for generated C. */
#pragma once
#include "cpu_state.h"
""")

    print("[1/4] Created the starter bank configuration.")

    analyzer = ROOT / "recompiler-rs" / "target" / "release" / (
        "snesrecomp-analyze.exe" if os.name == "nt" else "snesrecomp-analyze")
    if not analyzer.is_file():
        raise RuntimeError("the packaged native analyzer is missing")
    os.environ["SNESRECOMP_NATIVE_ANALYZER"] = str(analyzer)

    print("[2/4] Analyzing the ROM and generating C source...")
    if run_tool(v2_emit, [
        "--rom", str(rom_path),
        "--cfg-dir", str(config_dir),
        "--out-dir", str(generated_dir),
        "--analysis-backend", "native",
        "--no-host-root-scan",
    ]):
        raise RuntimeError("source generation failed")

    print("[3/4] Copying the integration framework...")
    runner_source = ROOT / "framework" / "runner"
    if not runner_source.is_dir():
        runner_source = ROOT / "runner"
    if not (runner_source / "runner.cmake").is_file():
        raise RuntimeError("the packaged runner framework is missing")
    shutil.copytree(runner_source, output / "snesrecomp" / "runner")

    cmake = f"""cmake_minimum_required(VERSION 3.20)
project({project_name} C)
set(CMAKE_C_STANDARD 11)

file(GLOB GENERATED_SOURCES CONFIGURE_DEPENDS
  "${{CMAKE_CURRENT_SOURCE_DIR}}/generated/*.c")
add_library(snesrecomp_game STATIC ${{GENERATED_SOURCES}})
target_include_directories(snesrecomp_game PRIVATE
  "${{CMAKE_CURRENT_SOURCE_DIR}}/config"
  "${{CMAKE_CURRENT_SOURCE_DIR}}/snesrecomp/runner/src"
  "${{CMAKE_CURRENT_SOURCE_DIR}}/snesrecomp/runner/src/snes")
if(NOT MSVC)
  target_compile_options(snesrecomp_game PRIVATE
    -w -Wno-implicit-function-declaration)
endif()
"""
    write_text(output / "CMakeLists.txt", cmake)
    write_text(output / "build.ps1", """$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
cmake -S $Root -B (Join-Path $Root 'build') -G Ninja -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
cmake --build (Join-Path $Root 'build') --config Release --parallel
exit $LASTEXITCODE
""")
    write_text(output / "build.sh", """#!/usr/bin/env sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cmake -S "$ROOT" -B "$ROOT/build" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$ROOT/build" --config Release --parallel
""")
    write_text(output / ".gitignore", "build/\ngenerated/\n")
    write_text(output / "project.txt", (
        f"name={title}\n"
        f"rom_file={rom_path.name}\n"
        f"rom_sha256={hashlib.sha256(raw).hexdigest()}\n"
        f"normalized_size={len(normalized_rom)}\n"
        f"mapping={mapping}\n"
    ))
    write_text(output / "README.md", f"""# {title} recompilation project

Generated locally from your ROM by snesrecomp.

## Build the generated source

Install CMake, Ninja, and a C compiler. On Windows, run:

```powershell
.\\build.ps1
```

On macOS or Linux, run `sh build.sh`.

The result is a static library named `snesrecomp_game`. It contains the
automatically discovered recompiled code. The original ROM is not copied into
this project.

## Continue the port

An arbitrary SNES game still needs game-specific function boundaries,
indirect-dispatch configuration, and a host application before it is a
playable native port. Add those declarations under `config/`, regenerate the
source, and integrate the library with the runner under `snesrecomp/runner`.

`generated/` is derived from copyrighted ROM data. Do not redistribute it
unless you have permission.
""")
    print("[4/4] Wrote project files.")
    print(f"\nReady: {output}")
    print(f"Build with: {output / ('build.ps1' if os.name == 'nt' else 'build.sh')}")
    return 0


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(
        prog="snesrecomp",
        description="Turn a SNES ROM into a recompilation source project.")
    commands = result.add_subparsers(dest="command", required=True)
    build = commands.add_parser(
        "build", help="generate C source and build scripts from a ROM")
    build.add_argument("--rom", required=True, help="path to a .sfc or .smc ROM")
    build.add_argument("--output", "-o", required=True, help="new output directory")
    build.add_argument("--name", help="project title (defaults to the ROM filename)")
    build.set_defaults(handler=build_project)
    return result


def main() -> int:
    arguments = parser().parse_args()
    try:
        return arguments.handler(arguments)
    except (OSError, RuntimeError, ValueError) as exc:
        print(f"snesrecomp: error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
