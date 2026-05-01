"""Shared MSVC compile-and-run pipeline for the fuzz harnesses.

Every fuzz target (v1 single-insn, v2 stale-shadow, future classes)
generates a self-contained C source, compiles it with cl.exe via the
vcvars64 environment, runs the resulting exe, and parses JSON-line
post-state output. This module captures that pipeline once.

Public surface:

    compile_c_to_exe(src: str, *, work_dir: pathlib.Path | None = None,
                     exe_name: str = 'fuzz.exe') -> pathlib.Path
        Writes `src` to a temp dir, compiles it via vcvars64 + cl.exe,
        returns the path to the produced exe. Raises BuildError on
        compilation failure (with stdout/stderr captured for the
        caller to surface).

    run_capturing_jsonl(exe: pathlib.Path,
                        *args: str) -> tuple[list[dict], int]
        Runs the exe, parses each line of stdout as JSON, returns
        `(results, returncode)`. Lines that fail to parse are recorded
        as `{'_parse_error': line}` so callers can spot harness bugs.

The vcvars64 path is fixed for VS 2022 Community on the project's
build host; override via `SNESRECOMP_VCVARS64` env if a different VS
install is in use.
"""
from __future__ import annotations

import json
import os
import pathlib
import subprocess
import tempfile
from typing import List, Tuple, Optional


DEFAULT_VCVARS64 = (
    r"C:\Program Files\Microsoft Visual Studio\2022\Community"
    r"\VC\Auxiliary\Build\vcvars64.bat"
)


class BuildError(RuntimeError):
    """cl.exe failed. Carries stdout/stderr for the caller to print."""

    def __init__(self, stdout: str, stderr: str, src_path: pathlib.Path):
        super().__init__(f"cl.exe build failed (source preserved at {src_path})")
        self.stdout = stdout
        self.stderr = stderr
        self.src_path = src_path


def _vcvars_path() -> str:
    return os.environ.get('SNESRECOMP_VCVARS64', DEFAULT_VCVARS64)


def compile_c_to_exe(src: str, *,
                     work_dir: Optional[pathlib.Path] = None,
                     exe_name: str = 'fuzz.exe',
                     extra_cl_flags: str = '/nologo /O2') -> pathlib.Path:
    """Write `src` to a temp dir and compile to an exe via cl.exe.

    Returns the path to the produced exe. Raises BuildError on failure.
    The work directory is preserved on failure (the source is kept) so
    the caller can re-read it for diagnostics; it is NOT auto-cleaned
    even on success — fuzz inspection often wants to look at the
    generated C after the run.
    """
    if work_dir is None:
        work_dir = pathlib.Path(tempfile.mkdtemp(prefix='snesrecomp_fuzz_'))
    work_dir.mkdir(parents=True, exist_ok=True)

    src_path = work_dir / 'fuzz.c'
    exe_path = work_dir / exe_name
    src_path.write_text(src, encoding='utf-8')

    # vcvars64 invokes vswhere.exe; ensure its location is on PATH even
    # when cmd.exe is launched from environments (e.g. Git Bash) that
    # strip the default Windows PATH entries.
    bat_path = work_dir / 'build.bat'
    bat_path.write_text(
        f'@echo off\n'
        f'set "PATH=C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer;'
        f'C:\\Windows\\System32;C:\\Windows;%PATH%"\n'
        f'call "{_vcvars_path()}" >NUL\n'
        # cl outputs object/exe info to stdout; we want stderr too for
        # diagnostics on failure. Don't suppress.
        f'cl {extra_cl_flags} /Fe:"{exe_path}" "{src_path}"\n'
        f'exit /b %ERRORLEVEL%\n',
        encoding='utf-8',
    )

    rc = subprocess.run(['cmd', '/c', str(bat_path)],
                        capture_output=True, text=True)
    if rc.returncode != 0:
        raise BuildError(rc.stdout, rc.stderr, src_path)
    return exe_path


def run_capturing_jsonl(exe: pathlib.Path,
                        *args: str) -> Tuple[List[dict], int]:
    """Run `exe` with optional args and parse each stdout line as JSON.

    Returns `(results, returncode)`. Any line that fails to parse as
    JSON shows up in results as `{'_parse_error': '<line>'}` rather
    than raising — callers usually want to surface the malformed line
    in their diagnostic output rather than abort.
    """
    rc = subprocess.run([str(exe), *args], capture_output=True, text=True)
    results: List[dict] = []
    for line in rc.stdout.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            results.append(json.loads(line))
        except json.JSONDecodeError:
            results.append({'_parse_error': line})
    return results, rc.returncode
