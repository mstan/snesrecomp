"""Headless ROM → C generate orchestration for UIs and launchers.

Stable entry points:
  python snesrecomp_cli.py generate ...
  python snesrecomp_cli.py verify-rom ...

Exit codes:
  0  success
  1  runtime / generation failure
  2  usage / argument error
  3  ROM verification failure
"""

from __future__ import annotations

import argparse
import contextlib
import io
import os
import pathlib
import sys
from typing import Optional, Sequence

from tools.sdk_progress import ProgressReporter
from tools.sdk_rom import RomVerifyError, verify_rom


EXIT_OK = 0
EXIT_ERROR = 1
EXIT_USAGE = 2
EXIT_VERIFY = 3


def run_tool(tool, arguments: Sequence[str]) -> int:
    """Invoke a tools.* module ``main()`` with a temporary ``sys.argv``."""
    original = sys.argv
    try:
        sys.argv = [getattr(tool, "__file__", "tool"), *arguments]
        try:
            result = tool.main()
        except SystemExit as exc:
            result = exc.code
        return int(result or 0)
    finally:
        sys.argv = original


def _resolve_under(root: Optional[pathlib.Path], value: str) -> pathlib.Path:
    path = pathlib.Path(value).expanduser()
    if not path.is_absolute() and root is not None:
        path = root / path
    return path.resolve()


def generate(
    *,
    rom: pathlib.Path,
    cfg_dir: pathlib.Path,
    out_dir: pathlib.Path,
    funcs_h: Optional[pathlib.Path] = None,
    cfg_roots: bool = False,
    no_host_root_scan: bool = False,
    source_roots: Optional[Sequence[pathlib.Path]] = None,
    analysis_backend: str = "auto",
    expected_crc32: Optional[str] = None,
    expected_sha256: Optional[str] = None,
    progress: Optional[ProgressReporter] = None,
) -> dict:
    """Verify a ROM, emit C into ``out_dir``, optionally sync ``funcs.h``."""
    from tools import v2_emit, v2_sync_funcs_h

    reporter = progress or ProgressReporter()
    rom = pathlib.Path(rom).resolve()
    cfg_dir = pathlib.Path(cfg_dir).resolve()
    out_dir = pathlib.Path(out_dir).resolve()
    if funcs_h is not None:
        funcs_h = pathlib.Path(funcs_h).resolve()

    if not cfg_dir.is_dir():
        raise ValueError(f"cfg-dir is not a directory: {cfg_dir}")
    if not any(cfg_dir.glob("bank*.cfg")):
        raise ValueError(f"cfg-dir has no bank*.cfg files: {cfg_dir}")

    reporter.phase("verify", pct=0.05, message=f"Verifying ROM {rom.name}")
    identity = verify_rom(
        rom,
        expected_crc32=expected_crc32,
        expected_sha256=expected_sha256,
    )
    reporter.event(
        "rom",
        path=identity["path"],
        crc32=identity["crc32"],
        sha256=identity["sha256"],
        size=identity["size"],
        verified=identity.get("verified", False),
    )

    out_dir.mkdir(parents=True, exist_ok=True)
    emit_args = [
        "--rom", str(rom),
        "--cfg-dir", str(cfg_dir),
        "--out-dir", str(out_dir),
        "--analysis-backend", analysis_backend,
    ]
    if cfg_roots:
        emit_args.append("--cfg-roots")
    if no_host_root_scan:
        emit_args.append("--no-host-root-scan")
    for root in source_roots or ():
        emit_args.extend(["--source-root", str(pathlib.Path(root).resolve())])

    def run_captured(tool, arguments: Sequence[str]) -> int:
        """Run a tools.* main(), keeping stdout JSONL-clean when needed."""
        if not reporter.json_progress:
            return run_tool(tool, arguments)
        capture = io.StringIO()
        with contextlib.redirect_stdout(capture):
            code = run_tool(tool, arguments)
        for line in capture.getvalue().splitlines():
            if line.strip():
                reporter.log(line)
        return code

    reporter.phase(
        "emit",
        pct=0.15,
        message=f"Analyzing ROM and generating C into {out_dir}",
    )
    code = run_captured(v2_emit, emit_args)
    if code != 0:
        raise RuntimeError(f"source generation failed (exit {code})")

    funcs_count = None
    if funcs_h is not None:
        reporter.phase(
            "sync_funcs_h",
            pct=0.9,
            message=f"Syncing function declarations to {funcs_h}",
        )
        sync_code = run_captured(
            v2_sync_funcs_h,
            ["--cfg-dir", str(cfg_dir), "--out", str(funcs_h)],
        )
        if sync_code != 0:
            raise RuntimeError(f"funcs.h sync failed (exit {sync_code})")
        try:
            text = funcs_h.read_text(encoding="utf-8")
            for line in text.splitlines():
                if line.startswith("/* ") and " functions across all banks" in line:
                    funcs_count = int(line.split()[1])
                    break
        except (OSError, ValueError):
            funcs_count = None

    result = {
        "ok": True,
        "rom": identity,
        "cfg_dir": str(cfg_dir),
        "out_dir": str(out_dir),
        "funcs_h": str(funcs_h) if funcs_h is not None else None,
        "funcs_count": funcs_count,
        "cfg_roots": bool(cfg_roots),
        "analysis_backend": analysis_backend,
    }
    reporter.phase("done", pct=1.0, message="Generate complete")
    reporter.result(**result)
    return result


def verify_rom_command(args: argparse.Namespace, progress: ProgressReporter) -> int:
    try:
        identity = verify_rom(
            pathlib.Path(args.rom),
            expected_crc32=args.expected_crc32,
            expected_sha256=args.expected_sha256,
        )
    except RomVerifyError as exc:
        progress.error(str(exc), code=EXIT_VERIFY, details=exc.details)
        if progress.json_progress and exc.details:
            progress.event("verify_failed", **exc.details)
        elif not progress.json_progress and exc.details:
            print(f"details: {exc.details}", file=sys.stderr)
        return EXIT_VERIFY
    except (OSError, ValueError) as exc:
        progress.error(str(exc), code=EXIT_ERROR)
        return EXIT_ERROR

    progress.phase("done", pct=1.0, message="ROM verification succeeded")
    progress.result(ok=True, rom=identity)
    if not progress.json_progress:
        print(
            f"ok crc32={identity['crc32']} sha256={identity['sha256']} "
            f"size={identity['size']} path={identity['path']}"
        )
    return EXIT_OK


def generate_command(args: argparse.Namespace, progress: ProgressReporter) -> int:
    project_root = None
    if args.project_root:
        project_root = pathlib.Path(args.project_root).expanduser().resolve()
        if not project_root.is_dir():
            progress.error(
                f"project-root is not a directory: {project_root}",
                code=EXIT_USAGE,
            )
            return EXIT_USAGE

    try:
        generate(
            rom=_resolve_under(project_root, args.rom),
            cfg_dir=_resolve_under(project_root, args.cfg_dir),
            out_dir=_resolve_under(project_root, args.out_dir),
            funcs_h=(
                _resolve_under(project_root, args.funcs_h)
                if args.funcs_h else None
            ),
            cfg_roots=bool(args.cfg_roots),
            no_host_root_scan=bool(args.no_host_root_scan),
            source_roots=[
                _resolve_under(project_root, root) for root in args.source_root
            ],
            analysis_backend=args.analysis_backend,
            expected_crc32=args.expected_crc32,
            expected_sha256=args.expected_sha256,
            progress=progress,
        )
    except RomVerifyError as exc:
        progress.error(str(exc), code=EXIT_VERIFY, details=exc.details)
        if progress.json_progress and exc.details:
            progress.event("verify_failed", **exc.details)
        return EXIT_VERIFY
    except (OSError, RuntimeError, ValueError) as exc:
        progress.error(str(exc), code=EXIT_ERROR)
        return EXIT_ERROR
    return EXIT_OK


def add_identity_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--expected-crc32",
        help="optional CRC32 (8 hex digits, optional 0x prefix)",
    )
    parser.add_argument(
        "--expected-sha256",
        help="optional SHA-256 (64 hex digits)",
    )


def add_generate_parser(subparsers) -> None:
    generate_parser = subparsers.add_parser(
        "generate",
        help="regenerate C sources for an existing recomp project from a ROM",
    )
    generate_parser.add_argument("--rom", required=True, help="path to .sfc/.smc ROM")
    generate_parser.add_argument(
        "--cfg-dir",
        required=True,
        help="directory containing bank*.cfg analysis seeds",
    )
    generate_parser.add_argument(
        "--out-dir",
        required=True,
        help="directory for generated C (created if missing)",
    )
    generate_parser.add_argument(
        "--project-root",
        help="resolve relative --rom/--cfg-dir/--out-dir/--funcs-h paths here",
    )
    generate_parser.add_argument(
        "--funcs-h",
        help="optional path to rewrite with v2_sync_funcs_h.py",
    )
    generate_parser.add_argument(
        "--cfg-roots",
        action="store_true",
        help="seed analysis from every cfg func declaration",
    )
    generate_parser.add_argument(
        "--no-host-root-scan",
        action="store_true",
        help="do not scan host sources for additional AOT roots",
    )
    generate_parser.add_argument(
        "--source-root",
        action="append",
        default=[],
        help="extra host source root for root discovery (repeatable)",
    )
    generate_parser.add_argument(
        "--analysis-backend",
        choices=("auto", "python", "native"),
        default="auto",
        help="whole-program analyzer (default: auto)",
    )
    add_identity_args(generate_parser)
    generate_parser.add_argument(
        "--json-progress",
        action="store_true",
        help="emit JSONL progress/result events on stdout",
    )
    generate_parser.set_defaults(handler=generate_command)


def add_verify_parser(subparsers) -> None:
    verify_parser = subparsers.add_parser(
        "verify-rom",
        help="check ROM shape and optional expected digests",
    )
    verify_parser.add_argument("--rom", required=True, help="path to .sfc/.smc ROM")
    add_identity_args(verify_parser)
    verify_parser.add_argument(
        "--json-progress",
        action="store_true",
        help="emit JSONL progress/result events on stdout",
    )
    verify_parser.set_defaults(handler=verify_rom_command)


def ensure_tool_paths(root: pathlib.Path) -> None:
    os.environ.setdefault("SNESRECOMP_ROOT", str(root))
    for path in (root, root / "recompiler", root / "tools"):
        value = str(path)
        if value not in sys.path:
            sys.path.insert(0, value)
