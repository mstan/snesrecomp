"""Self-contained ROM-to-source front end for snesrecomp."""

from __future__ import annotations

import argparse
import hashlib
import os
import pathlib
import re
import shutil
import sys
import zlib


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


ROM_SUFFIXES = (".sfc", ".smc")


def read_rom(path: pathlib.Path) -> bytes:
    """Validate ROM shape and return its bytes, or raise ValueError."""
    if not path.is_file():
        raise ValueError(f"ROM not found: {path}")
    if path.suffix.lower() not in ROM_SUFFIXES:
        raise ValueError("ROM must be an .sfc or .smc file")
    raw = path.read_bytes()
    if len(raw) < 32 * 1024 or len(raw) > 16 * 1024 * 1024:
        raise ValueError("ROM size is outside the supported 32 KiB to 16 MiB range")
    if len(raw) % 1024 not in (0, 512):
        raise ValueError("ROM size is not a standard SNES image size")
    return raw


def rom_digests(raw: bytes) -> tuple[str, str]:
    """(crc32, sha256) as lowercase hex, over the file exactly as supplied."""
    return ("%08x" % (zlib.crc32(raw) & 0xFFFFFFFF), hashlib.sha256(raw).hexdigest())


def check_expected_digests(raw: bytes, expected_crc32: str | None,
                           expected_sha256: str | None) -> None:
    crc, sha = rom_digests(raw)
    if expected_crc32 and crc.lower() != expected_crc32.strip().lower():
        raise ValueError(
            f"ROM CRC32 {crc} does not match the expected {expected_crc32.lower()}")
    if expected_sha256 and sha.lower() != expected_sha256.strip().lower():
        raise ValueError(
            f"ROM SHA-256 {sha} does not match the expected {expected_sha256.lower()}")


def resolve_analyzer(backend: str) -> str:
    """Point the emitter at the native analyzer when one is available.

    `auto` uses the native analyzer if it is built and the Python analyzer
    otherwise; `native` insists and fails loudly when it is missing, because
    silently dropping to a different analyzer would change what gets emitted.
    """
    analyzer = ROOT / "recompiler-rs" / "target" / "release" / (
        "snesrecomp-analyze.exe" if os.name == "nt" else "snesrecomp-analyze")
    if analyzer.is_file():
        os.environ["SNESRECOMP_NATIVE_ANALYZER"] = str(analyzer)
        return backend if backend != "auto" else "native"
    if backend == "native":
        raise RuntimeError(
            "--analysis-backend native was requested but the analyzer is not "
            f"built at {analyzer} (build it with "
            "tools/build_native_analyzer.py)")
    return "python" if backend == "auto" else backend


def run_emit(rom: pathlib.Path, cfg_dir: pathlib.Path, out_dir: pathlib.Path,
             *, backend: str = "auto", cfg_roots: bool = False,
             no_host_root_scan: bool = False,
             source_roots: list[str] | None = None) -> None:
    """Generate C from a ROM plus its bank configs. Raises on failure."""
    resolved = resolve_analyzer(backend)
    arguments = [
        "--rom", str(rom),
        "--cfg-dir", str(cfg_dir),
        "--out-dir", str(out_dir),
        "--analysis-backend", resolved,
    ]
    if cfg_roots:
        arguments.append("--cfg-roots")
    if no_host_root_scan:
        arguments.append("--no-host-root-scan")
    for root in source_roots or []:
        arguments.extend(["--source-root", root])
    if run_tool(v2_emit, arguments):
        raise RuntimeError("source generation failed")


def generate_project(args: argparse.Namespace) -> int:
    """Regenerate an existing project's C sources in place.

    The contract (flag names and meanings) matches the local codegen SDK on
    feat/local-codegen-sdk, so a project's tools/regen.sh works against either
    without changing. That branch's richer implementation adds JSONL progress;
    this one is the same pipeline without it.
    """
    base = pathlib.Path(args.project_root).expanduser().resolve() \
        if args.project_root else pathlib.Path.cwd()

    def under(value: str) -> pathlib.Path:
        path = pathlib.Path(value).expanduser()
        return path.resolve() if path.is_absolute() else (base / path).resolve()

    rom = under(args.rom)
    cfg_dir = under(args.cfg_dir)
    out_dir = under(args.out_dir)

    raw = read_rom(rom)
    check_expected_digests(raw, args.expected_crc32, args.expected_sha256)
    if not cfg_dir.is_dir():
        raise ValueError(f"config directory not found: {cfg_dir}")
    out_dir.mkdir(parents=True, exist_ok=True)

    crc, _ = rom_digests(raw)
    print(f"snesrecomp: generating from {rom.name} (crc32 {crc})")
    run_emit(rom, cfg_dir, out_dir,
             backend=args.analysis_backend,
             cfg_roots=args.cfg_roots,
             no_host_root_scan=args.no_host_root_scan,
             source_roots=args.source_root)

    if args.funcs_h:
        funcs_h = under(args.funcs_h)
        try:
            from tools import v2_sync_funcs_h
        except ImportError:
            print("snesrecomp: warning: v2_sync_funcs_h unavailable; "
                  f"left {funcs_h} untouched", file=sys.stderr)
        else:
            funcs_h.parent.mkdir(parents=True, exist_ok=True)
            if run_tool(v2_sync_funcs_h,
                        ["--cfg-dir", str(cfg_dir), "--out", str(funcs_h)]):
                raise RuntimeError(f"failed to sync {funcs_h}")
            print(f"snesrecomp: synced {funcs_h}")
    print(f"snesrecomp: generated {out_dir}")
    return 0


def verify_rom(args: argparse.Namespace) -> int:
    """Check ROM shape, and digests when the caller states expectations."""
    rom = pathlib.Path(args.rom).expanduser().resolve()
    raw = read_rom(rom)
    check_expected_digests(raw, args.expected_crc32, args.expected_sha256)
    crc, sha = rom_digests(raw)
    normalized = load_rom(str(rom))
    print(f"rom={rom.name}")
    print(f"size={len(raw)}")
    print(f"normalized_size={len(normalized)}")
    print(f"mapping={detect_rom_mapping(normalized)}")
    print(f"crc32={crc}")
    print(f"sha256={sha}")
    return 0


def build_project(args: argparse.Namespace) -> int:
    rom_path = pathlib.Path(args.rom).expanduser().resolve()
    output = pathlib.Path(args.output).expanduser().resolve()
    raw = read_rom(rom_path)
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

    print("[2/4] Analyzing the ROM and generating C source...")
    run_emit(rom_path, config_dir, generated_dir,
             backend="native", no_host_root_scan=True)

    print("[3/4] Copying the integration framework...")
    runner_source = ROOT / "framework" / "runner"
    if not runner_source.is_dir():
        runner_source = ROOT / "runner"
    if not (runner_source / "runner.cmake").is_file():
        raise RuntimeError("the packaged runner framework is missing")
    framework_output = output / "snesrecomp"
    shutil.copytree(runner_source, framework_output / "runner")
    framework_root = ROOT / "framework"
    if not (framework_root / "LICENSE").is_file():
        framework_root = ROOT
    shutil.copy2(framework_root / "LICENSE", framework_output)
    shutil.copy2(
        framework_root / "THIRD_PARTY_ATTRIBUTION.md", framework_output)

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
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host ''
Write-Host 'Built the generated-code static library.'
Write-Host 'No playable executable was produced; see README.md under "Continue the port".'
exit 0
""")
    write_text(output / "build.sh", """#!/usr/bin/env sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cmake -S "$ROOT" -B "$ROOT/build" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$ROOT/build" --config Release --parallel
printf '\n%s\n' 'Built the generated-code static library.'
printf '%s\n' 'No playable executable was produced; see README.md under "Continue the port".'
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

**Expected build result:** a static library named `snesrecomp_game`, not a
playable executable. The library contains the automatically discovered
recompiled code. The original ROM is not copied into this project.

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
    print("Expected build result: generated-code static library only.")
    print("A playable executable requires game-specific host integration; "
          "see the generated README.md.")
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

    def add_identity_args(sub):
        sub.add_argument("--expected-crc32",
                         help="fail unless the ROM CRC32 matches")
        sub.add_argument("--expected-sha256",
                         help="fail unless the ROM SHA-256 matches")

    generate = commands.add_parser(
        "generate",
        help="regenerate C sources for an existing recomp project from a ROM")
    generate.add_argument("--rom", required=True, help="path to a .sfc or .smc ROM")
    generate.add_argument("--cfg-dir", required=True,
                          help="directory containing bank*.cfg analysis seeds")
    generate.add_argument("--out-dir", required=True,
                          help="directory for generated C (created if missing)")
    generate.add_argument("--project-root",
                          help="resolve relative paths against this directory")
    generate.add_argument("--funcs-h",
                          help="optional header to re-sync with the generated C")
    generate.add_argument("--cfg-roots", action="store_true",
                          help="seed analysis from every cfg func declaration")
    generate.add_argument("--no-host-root-scan", action="store_true",
                          help="do not scan host sources for additional AOT roots")
    generate.add_argument("--source-root", action="append", default=[],
                          help="extra host source root for root discovery (repeatable)")
    generate.add_argument("--analysis-backend",
                          choices=("auto", "python", "native"), default="auto",
                          help="whole-program analyzer (default: auto)")
    add_identity_args(generate)
    generate.set_defaults(handler=generate_project)

    verify = commands.add_parser(
        "verify-rom", help="check ROM shape and optional expected digests")
    verify.add_argument("--rom", required=True, help="path to a .sfc or .smc ROM")
    add_identity_args(verify)
    verify.set_defaults(handler=verify_rom)

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
