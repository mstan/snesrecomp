#!/usr/bin/env python3
"""Prepare audio_trace retained-history variant relinks for title builds.

This intentionally starts from an explicit base linker response file, such as a
fresh dspgate RSP. It does not infer the link object set from build.ninja, so a
new audio candidate is not accidentally linked against stale pre-fix objects.
By default it writes compile/relink scripts and manifests only; pass --execute
only during a coordinated build/timing window.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import shlex
import shutil
import subprocess
from pathlib import Path


AUDIO_OBJECT_KEY = (
    "CMakeFiles/{target}.dir/F_/Projects/snesrecomp/snesrecomp/"
    "runner/src/audio_trace.c.obj"
)
AUDIO_SOURCE = Path("F:/Projects/snesrecomp/snesrecomp/runner/src/audio_trace.c")
HISTORY_VALUES = {
    "counters": "0",
    "small": "1",
    "full": "2",
    "reserved": "3",
}
DEFAULT_VARIANTS = ("counters", "full", "reserved")
CONFIG_FILES = ("config.ini", "config.local.ini", "rom.cfg", "keybinds.ini")
PAYLOAD_DIRS = ("assets", "mods", "saves")
MARKER = ".snesrecomp_audio_variant_stage"
WINDOWS_REPARSE_POINT = 0x400


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--title", required=True)
    p.add_argument("--target", required=True,
                   help="Target stem, for example SuperMarioWorldSNESRecomp.")
    p.add_argument("--build-dir", type=Path, required=True)
    p.add_argument("--base-rsp", type=Path,
                   help=("Fresh fixed-base linker RSP to mutate. If omitted, "
                         "derive the current object/library list from the "
                         "specified Ninja build."))
    p.add_argument("--out-root", type=Path, required=True)
    p.add_argument("--variant", action="append", choices=sorted(HISTORY_VALUES),
                   help=("Defaults to counters, full, and reserved; pass "
                         "--variant small explicitly for the reduced rings."))
    p.add_argument("--execute", action="store_true",
                   help="Compile and relink now. Default only prepares scripts.")
    return p.parse_args()


def file_meta(path: Path) -> dict:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    st = path.stat()
    return {
        "path": str(path),
        "bytes": st.st_size,
        "mtime_ns": st.st_mtime_ns,
        "sha256": h.hexdigest(),
    }


def maybe_file_meta(path: Path) -> dict:
    if not path.exists():
        return {"path": str(path), "exists": False}
    out = file_meta(path)
    out["exists"] = True
    return out


def is_reparse_point(path: Path) -> bool:
    try:
        return bool(path.stat(follow_symlinks=False).st_file_attributes &
                    WINDOWS_REPARSE_POINT)
    except AttributeError:
        return False


def is_link_like(path: Path) -> bool:
    return path.is_symlink() or is_reparse_point(path)


def is_relative_to(path: Path, base: Path) -> bool:
    try:
        path.resolve().relative_to(base.resolve())
        return True
    except ValueError:
        return False


def paths_overlap(a: Path, b: Path) -> bool:
    ar = a.resolve()
    br = b.resolve()
    return is_relative_to(ar, br) or is_relative_to(br, ar)


def is_root_dir(path: Path) -> bool:
    resolved = path.resolve()
    return resolved == resolved.anchor or resolved.parent == resolved


def validate_no_links_under(path: Path) -> None:
    if is_link_like(path):
        raise SystemExit(f"refusing symlink/junction/reparse point: {path}")
    if path.is_dir():
        for child in path.rglob("*"):
            if is_link_like(child):
                raise SystemExit(
                    f"refusing symlink/junction/reparse point: {child}")


def verify_tree_inside(path: Path, root: Path) -> None:
    root_resolved = root.resolve()
    if not is_relative_to(path, root_resolved):
        raise SystemExit(f"path escapes output directory: {path}")
    if path.is_dir():
        for child in path.rglob("*"):
            if is_link_like(child):
                raise SystemExit(
                    f"refusing symlink/junction/reparse point: {child}")
            if not is_relative_to(child, root_resolved):
                raise SystemExit(f"path escapes output directory: {child}")


def ensure_owned_directory(directory: Path) -> None:
    if is_root_dir(directory):
        raise SystemExit(f"refusing root output directory: {directory}")
    if directory.exists() and is_link_like(directory):
        raise SystemExit(f"refusing symlink/junction output directory: {directory}")
    directory.mkdir(parents=True, exist_ok=True)
    marker = directory / MARKER
    children = list(directory.iterdir())
    if children and not marker.exists():
        raise SystemExit(
            f"output directory is not empty and lacks {MARKER}: {directory}")
    marker.write_text("snesrecomp audio variant stage\n", encoding="utf-8")


def clear_owned_directory(directory: Path) -> None:
    marker = directory / MARKER
    if not marker.exists():
        raise SystemExit(f"refusing to clear unmarked output directory: {directory}")
    verify_tree_inside(directory, directory)
    for child in directory.iterdir():
        if child == marker:
            continue
        verify_tree_inside(child, directory)
        if child.is_dir():
            shutil.rmtree(child)
        else:
            child.unlink()


def parse_logical_ninja_lines(path: Path) -> list[str]:
    logical: list[str] = []
    pending = ""
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.rstrip()
        if line.endswith("$"):
            pending += line[:-1]
            continue
        logical.append(pending + line)
        pending = ""
    if pending:
        logical.append(pending)
    return logical


def split_ninja_words(text: str) -> list[str]:
    return shlex.split(text.replace("$:", ":").replace("$$", "$"),
                       posix=False)


def parse_build_edge(build_dir: Path, output: str) -> dict:
    ninja = build_dir / "build.ninja"
    lines = parse_logical_ninja_lines(ninja)
    prefix = f"build {output}:"
    for i, line in enumerate(lines):
        if not line.startswith(prefix):
            continue
        variables = {}
        j = i + 1
        while j < len(lines) and lines[j].startswith("  "):
            body = lines[j].strip()
            if " = " in body:
                key, value = body.split(" = ", 1)
                variables[key] = value
            j += 1
        return variables
    raise SystemExit(f"build edge not found: {output}")


def parse_link_edge(build_dir: Path, target: str) -> dict:
    ninja = build_dir / "build.ninja"
    lines = parse_logical_ninja_lines(ninja)
    prefix = f"build {target}.exe:"
    for i, line in enumerate(lines):
        if not line.startswith(prefix):
            continue
        words = split_ninja_words(line[len("build "):])
        explicit = []
        for word in words[2:]:
            if word in ("|", "||"):
                break
            if word.endswith(".obj"):
                explicit.append(word)
        variables = {}
        j = i + 1
        while j < len(lines) and lines[j].startswith("  "):
            body = lines[j].strip()
            if " = " in body:
                key, value = body.split(" = ", 1)
                variables[key] = value
            j += 1
        libs = split_ninja_words(variables.get("LINK_LIBRARIES", ""))
        if not explicit:
            raise SystemExit(f"no link objects found for {target}.exe")
        return {"objects": explicit, "libraries": libs}
    raise SystemExit(f"link edge not found: {target}.exe")


def cmake_cache_value(build_dir: Path, name: str) -> str | None:
    cache = build_dir / "CMakeCache.txt"
    prefix = f"{name}:"
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith(prefix):
            return line.split("=", 1)[1]
    return None


def ps_single_quote(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def write_ps_array(values: list[str]) -> str:
    return "@(\n" + "".join(f"  {ps_single_quote(v)}\n" for v in values) + ")"


def without_audio_history_define(defines: list[str]) -> list[str]:
    return [
        d for d in defines
        if not d.startswith("-DSNESRECOMP_AUDIO_TRACE_HISTORY=")
    ]


def copy_payload(build_dir: Path, out_dir: Path) -> None:
    for dll in sorted(build_dir.glob("*.dll")):
        validate_no_links_under(dll)
        shutil.copy2(dll, out_dir / dll.name)
    for name in CONFIG_FILES:
        src = build_dir / name
        if src.exists():
            validate_no_links_under(src)
            shutil.copy2(src, out_dir / name)
    for name in PAYLOAD_DIRS:
        src = build_dir / name
        if src.exists():
            validate_no_links_under(src)
            dst = out_dir / name
            if dst.exists():
                raise SystemExit(f"payload already exists: {dst}")
            shutil.copytree(src, dst)


def replace_audio_object_rsp(text: str, target: str, replacement: Path) -> str:
    key = AUDIO_OBJECT_KEY.format(target=target)
    replacement_text = str(replacement).replace("\\", "/")
    replaced = text.replace(key, replacement_text)
    if replaced == text:
        raise SystemExit(f"base RSP does not contain expected audio object: {key}")
    return replaced


def base_rsp_text(args: argparse.Namespace) -> str:
    if args.base_rsp:
        return args.base_rsp.read_text(encoding="utf-8")
    link_edge = parse_link_edge(args.build_dir, args.target)
    return " ".join([*link_edge["objects"], *link_edge["libraries"]]) + "\n"


def prepare_variant(args: argparse.Namespace, variant: str,
                    compile_vars: dict, c_compiler: str,
                    cxx_compiler: str) -> dict:
    out_dir = args.out_root / args.title / variant
    ensure_owned_directory(out_dir)
    clear_owned_directory(out_dir)

    audio_obj = out_dir / "audio_trace.c.obj"
    audio_dep = out_dir / "audio_trace.c.obj.d"
    exe_out = out_dir / f"{args.target}-{variant}.exe"
    object_rsp = out_dir / f"{args.target}.{variant}.rsp"
    object_rsp.write_text(
        replace_audio_object_rsp(base_rsp_text(args), args.target, audio_obj),
        encoding="utf-8")

    defines = without_audio_history_define(
        split_ninja_words(compile_vars.get("DEFINES", "")))
    defines.append(
        f"-DSNESRECOMP_AUDIO_TRACE_HISTORY={HISTORY_VALUES[variant]}")
    includes = split_ninja_words(compile_vars.get("INCLUDES", ""))
    flags = split_ninja_words(compile_vars.get("FLAGS", ""))
    compile_args = [
        *defines, *includes, *flags, "-MD", "-MT", str(audio_obj),
        "-MF", str(audio_dep), "-o", str(audio_obj), "-c", str(AUDIO_SOURCE),
    ]
    link_args = [
        "-O3", "-DNDEBUG", f"@{object_rsp}", "-o", str(exe_out),
        f"-Wl,--out-implib,{out_dir / ('lib' + args.target + '-' + variant + '.dll.a')}",
        "-Wl,--major-image-version,0,--minor-image-version,0",
    ]
    compile_ps1 = out_dir / "compile_audio.ps1"
    relink_ps1 = out_dir / "relink.ps1"
    compile_ps1.write_text(
        "$ErrorActionPreference = 'Stop'\n"
        f"$argv = {write_ps_array(compile_args)}\n"
        f"& {ps_single_quote(c_compiler)} @argv\n"
        "if ($LASTEXITCODE -ne 0) { throw \"audio compile failed\" }\n",
        encoding="utf-8")
    relink_ps1.write_text(
        "$ErrorActionPreference = 'Stop'\n"
        f"$argv = {write_ps_array(link_args)}\n"
        f"& {ps_single_quote(cxx_compiler)} @argv\n"
        "if ($LASTEXITCODE -ne 0) { throw \"relink failed\" }\n",
        encoding="utf-8")
    copy_payload(args.build_dir, out_dir)

    manifest = {
        "title": args.title,
        "target": args.target,
        "variant": variant,
        "prepared_only": not args.execute,
        "build_dir": str(args.build_dir),
        "base_rsp": (
            file_meta(args.base_rsp) if args.base_rsp else {
                "generated_from_build_ninja": True,
                "build_dir": str(args.build_dir),
            }
        ),
        "source": maybe_file_meta(AUDIO_SOURCE),
        "output_exe": str(exe_out),
        "audio_object": str(audio_obj),
        "object_rsp": str(object_rsp),
        "compile_ps1": str(compile_ps1),
        "relink_ps1": str(relink_ps1),
        "compile_command": [c_compiler, *compile_args],
        "relink_command": [cxx_compiler, *link_args],
        "payload_inputs": {
            name: maybe_file_meta(out_dir / name)
            for name in CONFIG_FILES
        },
        "payload_dlls": [file_meta(path) for path in sorted(out_dir.glob("*.dll"))],
    }
    if args.execute:
        subprocess.run([c_compiler, *compile_args], cwd=str(args.build_dir),
                       check=True)
        subprocess.run([cxx_compiler, *link_args], cwd=str(args.build_dir),
                       check=True)
        manifest["audio_object_meta"] = file_meta(audio_obj)
        manifest["output_exe_meta"] = file_meta(exe_out)
    (out_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


def validate_args(args: argparse.Namespace) -> None:
    args.build_dir = args.build_dir.resolve()
    if args.base_rsp:
        args.base_rsp = args.base_rsp.resolve()
    args.out_root = args.out_root.resolve()
    for label in ("build_dir",):
        path = getattr(args, label)
        if not path.exists():
            raise SystemExit(f"{label.replace('_', '-')} does not exist: {path}")
        if is_link_like(path):
            raise SystemExit(f"refusing symlink/junction: {path}")
    if args.base_rsp:
        if not args.base_rsp.exists():
            raise SystemExit(f"base-rsp does not exist: {args.base_rsp}")
        if is_link_like(args.base_rsp):
            raise SystemExit(f"refusing symlink/junction: {args.base_rsp}")
    if not args.build_dir.is_dir():
        raise SystemExit(f"--build-dir is not a directory: {args.build_dir}")
    if args.base_rsp and not args.base_rsp.is_file():
        raise SystemExit(f"--base-rsp is not a file: {args.base_rsp}")
    if is_root_dir(args.out_root):
        raise SystemExit(f"refusing root out-root: {args.out_root}")
    if paths_overlap(args.build_dir, args.out_root):
        raise SystemExit("build directory overlaps out-root")


def main() -> int:
    args = parse_args()
    validate_args(args)
    variants = args.variant or list(DEFAULT_VARIANTS)
    object_key = AUDIO_OBJECT_KEY.format(target=args.target)
    compile_vars = parse_build_edge(args.build_dir, object_key)
    c_compiler = cmake_cache_value(args.build_dir, "CMAKE_C_COMPILER")
    cxx_compiler = cmake_cache_value(args.build_dir, "CMAKE_CXX_COMPILER")
    if not c_compiler or not cxx_compiler:
        raise SystemExit("C/CXX compiler missing from CMakeCache.txt")
    manifests = [
        prepare_variant(args, variant, compile_vars, c_compiler, cxx_compiler)
        for variant in variants
    ]
    print(json.dumps({
        "title": args.title,
        "target": args.target,
        "execute": args.execute,
        "variants": manifests,
    }, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
