"""Self-contained ROM-to-source front end for snesrecomp."""

from __future__ import annotations

import argparse
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
from tools.sdk_generate import (  # noqa: E402
    EXIT_ERROR,
    add_generate_parser,
    add_verify_parser,
    run_tool,
)
from tools.sdk_progress import ProgressReporter  # noqa: E402
from tools.sdk_rom import RomVerifyError, verify_rom  # noqa: E402


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


def build_project(args: argparse.Namespace) -> int:
    rom_path = pathlib.Path(args.rom).expanduser().resolve()
    output = pathlib.Path(args.output).expanduser().resolve()
    try:
        identity = verify_rom(rom_path)
    except RomVerifyError as exc:
        raise ValueError(str(exc)) from exc
    raw = rom_path.read_bytes()
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

    # Released CLI builds bundle the native analyzer; a source checkout often
    # has not built it. Fall back to the Python analyzer rather than refusing
    # to run — the packaged path is unaffected because the binary is there.
    analyzer = ROOT / "recompiler-rs" / "target" / "release" / (
        "snesrecomp-analyze.exe" if os.name == "nt" else "snesrecomp-analyze")
    if analyzer.is_file():
        os.environ["SNESRECOMP_NATIVE_ANALYZER"] = str(analyzer)
        backend = "native"
    else:
        print("snesrecomp: native analyzer not built at "
              f"{analyzer};\n            using the Python analyzer "
              "(build it with tools/build_native_analyzer.py)")
        backend = "python"

    print("[2/4] Analyzing the ROM and generating C source...")
    if run_tool(v2_emit, [
        "--rom", str(rom_path),
        "--cfg-dir", str(config_dir),
        "--out-dir", str(generated_dir),
        "--analysis-backend", backend,
        "--no-host-root-scan",
    ]):
        raise RuntimeError("source generation failed")

    print("[3/4] Copying the integration framework...")
    runner_source = ROOT / "framework" / "runner"
    if not runner_source.is_dir():
        runner_source = ROOT / "runner"
    if not (runner_source / "runner.cmake").is_file():
        raise RuntimeError("the packaged runner framework is missing")
    framework_output = output / "snesrecomp"
    shutil.copytree(runner_source, framework_output / "runner")
    for notice in ("LICENSE", "THIRD_PARTY_ATTRIBUTION.md"):
        source = ROOT / notice
        if not source.is_file():
            source = ROOT / "framework" / notice
        if source.is_file():
            shutil.copy2(source, framework_output / notice)

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
if ($LASTEXITCODE -eq 0) {
    Write-Host 'No playable executable was produced; this build creates the generated-code static library only.'
}
exit $LASTEXITCODE
""")
    write_text(output / "build.sh", """#!/usr/bin/env sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cmake -S "$ROOT" -B "$ROOT/build" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$ROOT/build" --config Release --parallel
echo "No playable executable was produced; this build creates the generated-code static library only."
""")
    write_text(output / ".gitignore", "build/\ngenerated/\n")
    write_text(output / "project.txt", (
        f"name={title}\n"
        f"rom_file={rom_path.name}\n"
        f"rom_sha256={identity['sha256']}\n"
        f"rom_crc32={identity['crc32']}\n"
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

Expected build result: generated-code static library only. No playable executable is produced by this starter project.

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
    print("Expected build result: generated-code static library only")
    print(f"Build with: {output / ('build.ps1' if os.name == 'nt' else 'build.sh')}")
    return 0


def build_command(args: argparse.Namespace) -> int:
    return build_project(args)


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(
        prog="snesrecomp",
        description=(
            "Turn a SNES ROM into recompilation sources, or regenerate an "
            "existing project for UI / launcher automation."
        ),
    )
    commands = result.add_subparsers(dest="command", required=True)

    build = commands.add_parser(
        "build", help="scaffold a new project and generate C from a ROM")
    build.add_argument("--rom", required=True, help="path to a .sfc or .smc ROM")
    build.add_argument("--output", "-o", required=True, help="new output directory")
    build.add_argument("--name", help="project title (defaults to the ROM filename)")
    build.set_defaults(handler=build_command)

    add_generate_parser(commands)
    add_verify_parser(commands)
    return result


def main() -> int:
    arguments = parser().parse_args()
    handler = arguments.handler

    # SDK commands own their ProgressReporter / exit codes.
    if arguments.command in ("generate", "verify-rom"):
        progress = ProgressReporter(
            json_progress=bool(getattr(arguments, "json_progress", False)),
        )
        return handler(arguments, progress)

    try:
        return handler(arguments)
    except (OSError, RuntimeError, ValueError) as exc:
        print(f"snesrecomp: error: {exc}", file=sys.stderr)
        return EXIT_ERROR


if __name__ == "__main__":
    raise SystemExit(main())
