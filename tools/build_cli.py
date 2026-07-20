"""Build a self-contained snesrecomp command-line release archive."""

from __future__ import annotations

import argparse
import importlib.util
import os
import pathlib
import platform
import shutil
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parent.parent
ANALYZER_NAME = "snesrecomp-analyze.exe" if os.name == "nt" else "snesrecomp-analyze"


def run(command: list[str]) -> None:
    print("+", subprocess.list2cmdline(command), flush=True)
    subprocess.run(command, cwd=ROOT, check=True)


def platform_name() -> str:
    systems = {"Windows": "windows", "Linux": "linux", "Darwin": "macos"}
    machines = {
        "amd64": "x86_64",
        "x86_64": "x86_64",
        "arm64": "arm64",
        "aarch64": "arm64",
    }
    raw_system = platform.system()
    if raw_system.startswith(("MSYS", "MINGW", "CYGWIN")):
        raw_system = "Windows"
    return (
        f"{systems.get(raw_system, raw_system.lower())}-"
        f"{machines.get(platform.machine().lower(), platform.machine().lower())}"
    )


def data_argument(source: pathlib.Path, destination: str) -> str:
    return f"{source}{os.pathsep}{destination}"


def main() -> int:
    parser = argparse.ArgumentParser(
        description="build the self-contained snesrecomp CLI archive")
    parser.add_argument(
        "configuration", nargs="?", choices=("release", "debug"),
        default="release", help="analyzer configuration (default: release)")
    parser.add_argument(
        "--build-dir", default="build-cli",
        help="temporary build directory (default: build-cli)")
    parser.add_argument(
        "--dist-dir", default="dist",
        help="package output directory (default: dist)")
    parser.add_argument(
        "--skip-analyzer-build", action="store_true",
        help="package an analyzer that is already built")
    args = parser.parse_args()

    if importlib.util.find_spec("PyInstaller") is None:
        parser.error(
            "PyInstaller is not installed; run "
            "`python -m pip install pyinstaller==6.21.0`")

    profile = "release" if args.configuration == "release" else "debug"
    if not args.skip_analyzer_build:
        command = [sys.executable, str(ROOT / "tools" / "build_native_analyzer.py")]
        if profile == "debug":
            command.append("--debug")
        run(command)

    analyzer = ROOT / "recompiler-rs" / "target" / profile / ANALYZER_NAME
    if not analyzer.is_file():
        parser.error(f"native analyzer was not built at {analyzer}")

    build_dir = (ROOT / args.build_dir).resolve()
    dist_dir = (ROOT / args.dist_dir).resolve()
    pyinstaller_dist = build_dir / "pyinstaller-dist"
    pyinstaller_work = build_dir / "pyinstaller-work"
    spec_dir = build_dir / "spec"
    for directory in (pyinstaller_dist, pyinstaller_work, spec_dir):
        directory.mkdir(parents=True, exist_ok=True)

    add_data = [
        (ROOT / "runner", "framework/runner"),
        (ROOT / "recompiler", "recompiler"),
        (ROOT / "tools" / "v2_emit.py", "tools"),
        (ROOT / "tools" / "v2_analyze.py", "tools"),
        (ROOT / "tools" / "__init__.py", "tools"),
        (ROOT / "recompiler-rs" / "src", "recompiler-rs/src"),
        (ROOT / "recompiler-rs" / "Cargo.toml", "recompiler-rs"),
        (ROOT / "recompiler-rs" / "Cargo.lock", "recompiler-rs"),
    ]
    command = [
        sys.executable, "-m", "PyInstaller",
        "--noconfirm", "--clean", "--onedir", "--console", "--noupx",
        "--name", "snesrecomp",
        "--distpath", str(pyinstaller_dist),
        "--workpath", str(pyinstaller_work),
        "--specpath", str(spec_dir),
        "--paths", str(ROOT),
        "--paths", str(ROOT / "recompiler"),
        "--paths", str(ROOT / "tools"),
        "--hidden-import", "v2_analyze",
        "--exclude-module", "tkinter",
    ]
    for source, destination in add_data:
        command += ["--add-data", data_argument(source, destination)]
    command += [
        "--add-binary",
        data_argument(analyzer, "recompiler-rs/target/release"),
        str(ROOT / "snesrecomp_cli.py"),
    ]
    run(command)

    package = f"snesrecomp-cli-{platform_name()}"
    stage = dist_dir / package
    archive = dist_dir / f"{package}.zip"
    dist_dir.mkdir(parents=True, exist_ok=True)
    if stage.exists():
        shutil.rmtree(stage)
    if archive.exists():
        archive.unlink()
    shutil.copytree(pyinstaller_dist / "snesrecomp", stage)
    shutil.copy2(ROOT / "README.md", stage)
    shutil.copy2(ROOT / "THIRD_PARTY_ATTRIBUTION.md", stage)
    shutil.make_archive(str(archive.with_suffix("")), "zip", dist_dir, package)
    print(f"snesrecomp CLI package: {archive}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as exc:
        print(
            f"build_cli: command failed with exit code {exc.returncode}",
            file=sys.stderr)
        raise SystemExit(exc.returncode)
