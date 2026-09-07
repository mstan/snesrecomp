#!/usr/bin/env python3
"""Prepare object-substitution relink manifests for snesrecomp title builds.

The script reads an existing CMake/Ninja link edge, swaps selected object files
from a baseline build into a current build's object list, and writes response
files plus a PowerShell relink script. It does not link unless --execute is
explicitly passed.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shlex
import shutil
import subprocess
from pathlib import Path


MARKER = ".snesrecomp_object_variant_stage"
CONFIG_FILES = ("config.ini", "config.local.ini", "rom.cfg", "keybinds.ini")
PAYLOAD_DIRS = ("assets", "mods", "saves")
DEFAULT_VARIANTS = {
    "base_cpu_state": ("runner/src/cpu_state.c.obj",),
    "base_ppu": ("runner/src/snes/ppu.c.obj",),
    "orig_common_cpu_infra": ("runner/src/common_cpu_infra.c.obj",),
    "orig_common_rtl": ("runner/src/common_rtl.c.obj",),
    "orig_cpu_core_triple": (
        "runner/src/common_cpu_infra.c.obj",
        "runner/src/cpu_state.c.obj",
        "runner/src/common_rtl.c.obj",
    ),
}
WINDOWS_REPARSE_POINT = 0x400


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--title", required=True)
    p.add_argument("--target", required=True,
                   help="Executable target filename, for example Foo.exe.")
    p.add_argument("--current-build", type=Path, required=True)
    p.add_argument("--baseline-build", type=Path, required=True)
    p.add_argument("--out-root", type=Path, required=True)
    p.add_argument("--variant", action="append", choices=sorted(DEFAULT_VARIANTS),
                   help="Variant to prepare. Defaults to all requested variants.")
    p.add_argument("--object-override", action="append", default=[],
                   metavar="KEY=PATH",
                   help=("Use PATH for normalized object KEY instead of the "
                         "baseline build object, for example "
                         "runner/src/cpu_state.c.obj=F:/.../cpu.obj."))
    p.add_argument("--execute", action="store_true",
                   help="Actually invoke the generated relink commands.")
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
        attrs = path.stat(follow_symlinks=False).st_file_attributes
    except AttributeError:
        return False
    return bool(attrs & WINDOWS_REPARSE_POINT)


def is_link_like(path: Path) -> bool:
    return path.is_symlink() or is_reparse_point(path)


def is_relative_to(path: Path, base: Path) -> bool:
    try:
        path.resolve().relative_to(base.resolve())
    except ValueError:
        return False
    return True


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
    if not marker.exists():
        marker.write_text("snesrecomp object substitution variant stage\n",
                          encoding="utf-8")


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


def parse_link_edge(build_dir: Path, target: str) -> dict:
    ninja = build_dir / "build.ninja"
    if not ninja.exists():
        raise SystemExit(f"missing build.ninja: {ninja}")
    lines = parse_logical_ninja_lines(ninja)
    prefix = f"build {target}:"
    for i, line in enumerate(lines):
        if not line.startswith(prefix):
            continue
        words = split_ninja_words(line[len("build "):])
        # target:, rule, inputs...
        inputs = words[2:]
        explicit = []
        for word in inputs:
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
        if not explicit:
            raise SystemExit(f"no explicit .obj inputs found for {target}")
        return {"objects": explicit, "variables": variables}
    raise SystemExit(f"target link edge not found in {ninja}: {target}")


def normalized_obj_key(path: str) -> str:
    rel = path.replace("\\", "/")
    for token in ("runner/src/", "recomp-ui/src/", "src/", "overrides/",
                  "third_party/"):
        idx = rel.find(token)
        if idx >= 0:
            return rel[idx:]
    return rel


def collect_objects(build_dir: Path, target: str) -> dict[str, Path]:
    target_dir = build_dir / "CMakeFiles" / f"{target.removesuffix('.exe')}.dir"
    if not target_dir.exists():
        raise SystemExit(f"missing target object directory: {target_dir}")
    objects = {}
    for obj in target_dir.rglob("*.obj"):
        key = normalized_obj_key(str(obj))
        objects[key] = obj
    return objects


def parse_object_overrides(values: list[str]) -> dict[str, Path]:
    overrides = {}
    for value in values:
        if "=" not in value:
            raise SystemExit(f"invalid --object-override: {value}")
        key, raw_path = value.split("=", 1)
        key = key.replace("\\", "/")
        path = Path(raw_path).resolve()
        if not path.exists() or not path.is_file():
            raise SystemExit(f"object override is not a file: {path}")
        validate_no_links_under(path)
        overrides[key] = path
    return overrides


def cmake_cache_value(build_dir: Path, name: str) -> str | None:
    cache = build_dir / "CMakeCache.txt"
    if not cache.exists():
        return None
    prefix = f"{name}:"
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith(prefix):
            return line.split("=", 1)[1]
    return None


def quote_rsp(path: Path) -> str:
    return '"' + str(path).replace('"', '\\"') + '"'


def ps_single_quote(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def write_rsp(path: Path, entries: list[str]) -> None:
    path.write_text("\n".join(entries) + "\n", encoding="utf-8")


def copy_payload(current_build: Path, out_dir: Path) -> None:
    for dll in sorted(current_build.glob("*.dll")):
        validate_no_links_under(dll)
        shutil.copy2(dll, out_dir / dll.name)
    for name in CONFIG_FILES:
        src = current_build / name
        if src.exists():
            validate_no_links_under(src)
            shutil.copy2(src, out_dir / name)
    for name in PAYLOAD_DIRS:
        src = current_build / name
        if src.exists():
            validate_no_links_under(src)
            dst = out_dir / name
            if dst.exists():
                raise SystemExit(f"payload already exists: {dst}")
            shutil.copytree(src, dst)


def prepare_variant(args: argparse.Namespace, variant: str, keys: tuple[str, ...],
                    link_edge: dict, current_objects: dict[str, Path],
                    baseline_objects: dict[str, Path],
                    object_overrides: dict[str, Path] | None = None) -> dict:
    object_overrides = object_overrides or {}
    out_dir = args.out_root / args.title / variant
    ensure_owned_directory(out_dir)
    clear_owned_directory(out_dir)
    exe_out = out_dir / args.target
    objects = []
    substitutions = {}
    wanted = set(keys)
    for obj in link_edge["objects"]:
        key = normalized_obj_key(obj)
        current_path = (args.current_build / obj).resolve()
        selected = current_path
        source = "current"
        if key in wanted:
            selected = object_overrides.get(key) or baseline_objects.get(key)
            if selected is None:
                raise SystemExit(f"missing baseline object for {key}")
            validate_no_links_under(selected)
            source = "override" if key in object_overrides else "baseline"
            substitutions[key] = {
                "current": file_meta(current_objects[key]),
                source: file_meta(selected),
            }
        if not selected.exists():
            raise SystemExit(f"selected object missing: {selected}")
        validate_no_links_under(selected)
        objects.append(quote_rsp(selected))
        if source == "current" and key in current_objects:
            current_meta = file_meta(current_objects[key])
            resolved_meta = file_meta(selected)
            if current_meta["sha256"] != resolved_meta["sha256"]:
                raise SystemExit(f"current object mismatch for {key}: {selected}")
    libs = link_edge["variables"].get("LINK_LIBRARIES", "")
    flags = link_edge["variables"].get("FLAGS", "")
    cxx = cmake_cache_value(args.current_build, "CMAKE_CXX_COMPILER")
    if not cxx:
        raise SystemExit("CMAKE_CXX_COMPILER missing from current CMakeCache.txt")
    object_rsp = out_dir / "objects.rsp"
    libs_rsp = out_dir / "libs.rsp"
    write_rsp(object_rsp, objects)
    write_rsp(libs_rsp, split_ninja_words(libs))
    copy_payload(args.current_build, out_dir)
    command = [
        cxx,
        *split_ninja_words(flags),
        f"@{object_rsp}",
        "-o",
        str(exe_out),
        f"@{libs_rsp}",
    ]
    ps1 = out_dir / "relink.ps1"
    ps_args = [
        *split_ninja_words(flags),
        f"@{object_rsp}",
        "-o",
        str(exe_out),
        f"@{libs_rsp}",
    ]
    ps_array = "@(\n" + "".join(
        f"  {ps_single_quote(arg)}\n" for arg in ps_args) + ")"
    ps1.write_text(
        "$ErrorActionPreference = 'Stop'\n"
        f"$argv = {ps_array}\n"
        f"& {ps_single_quote(cxx)} @argv\n"
        "if ($LASTEXITCODE -ne 0) { throw \"relink failed\" }\n",
        encoding="utf-8")
    manifest = {
        "title": args.title,
        "variant": variant,
        "target": args.target,
        "prepared_only": not args.execute,
        "current_build": str(args.current_build),
        "baseline_build": str(args.baseline_build),
        "output_exe": str(exe_out),
        "compiler": cxx,
        "flags": split_ninja_words(flags),
        "link_libraries": split_ninja_words(libs),
        "object_rsp": str(object_rsp),
        "libs_rsp": str(libs_rsp),
        "relink_ps1": str(ps1),
        "command": command,
        "substitutions": substitutions,
        "payload_inputs": {
            name: maybe_file_meta(out_dir / name)
            for name in CONFIG_FILES
        },
        "payload_dlls": [
            file_meta(path) for path in sorted(out_dir.glob("*.dll"))
        ],
    }
    (out_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    if args.execute:
        subprocess.run(command, cwd=str(args.current_build), check=True)
    return manifest


def validate_args(args: argparse.Namespace) -> None:
    args.current_build = args.current_build.resolve()
    args.baseline_build = args.baseline_build.resolve()
    args.out_root = args.out_root.resolve()
    if is_root_dir(args.out_root):
        raise SystemExit(f"refusing root out-root: {args.out_root}")
    for label in ("current_build", "baseline_build"):
        path = getattr(args, label)
        if not path.exists() or not path.is_dir():
            raise SystemExit(f"{label.replace('_', '-')} is not a directory: {path}")
        if is_link_like(path):
            raise SystemExit(f"refusing symlink/junction directory: {path}")
    if paths_overlap(args.current_build, args.out_root):
        raise SystemExit("current build directory overlaps out-root")
    if paths_overlap(args.baseline_build, args.out_root):
        raise SystemExit("baseline build directory overlaps out-root")


def main() -> int:
    args = parse_args()
    validate_args(args)
    variants = args.variant or list(DEFAULT_VARIANTS)
    link_edge = parse_link_edge(args.current_build, args.target)
    current_objects = collect_objects(args.current_build, args.target)
    baseline_objects = collect_objects(args.baseline_build, args.target)
    object_overrides = parse_object_overrides(args.object_override)
    manifests = []
    for variant in variants:
        manifests.append(prepare_variant(
            args, variant, DEFAULT_VARIANTS[variant], link_edge,
            current_objects, baseline_objects, object_overrides))
    summary = {
        "title": args.title,
        "target": args.target,
        "execute": args.execute,
        "variants": manifests,
    }
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
