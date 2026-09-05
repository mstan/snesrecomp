#!/usr/bin/env python3
"""Run A/B benchmarks through one stable staged executable path.

This controls for SDL/driver/overlay behavior keyed by process image path. Each
run re-stages one source artifact directory into a marked scratch directory and
then invokes the title through the same staged executable path.
"""

from __future__ import annotations

import argparse
import json
import math
import shutil
from pathlib import Path

import run_benchmark_pairs as bench


MARKER = ".snesrecomp_same_path_stage"
CONFIG_FILES = ("config.ini", "config.local.ini", "rom.cfg", "keybinds.ini")
PAYLOAD_DIRS = ("assets", "mods", "saves")
WINDOWS_REPARSE_POINT = 0x400


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--frames", type=int, required=True)
    p.add_argument("--pairs", type=int, default=2)
    p.add_argument("--warmups", type=int, default=1)
    p.add_argument("--timeout", type=float, default=180.0)
    p.add_argument("--audio", action="store_true")
    p.add_argument("--stage-dir", type=Path, required=True)
    p.add_argument("--stage-exe-name",
                   help="Filename used inside stage-dir; defaults to A exe name.")
    p.add_argument("--a-name", default="A")
    p.add_argument("--a-exe", type=Path, required=True)
    p.add_argument("--a-rom", type=Path, required=True)
    p.add_argument("--b-name", default="B")
    p.add_argument("--b-exe", type=Path, required=True)
    p.add_argument("--b-rom", type=Path, required=True)
    p.add_argument("--json-out", type=Path)
    return p.parse_args()


def is_reparse_point(path: Path) -> bool:
    try:
        attrs = path.stat(follow_symlinks=False).st_file_attributes
    except AttributeError:
        return False
    return bool(attrs & WINDOWS_REPARSE_POINT)


def is_link_like(path: Path) -> bool:
    return path.is_symlink() or is_reparse_point(path)


def is_root_dir(path: Path) -> bool:
    resolved = path.resolve()
    return resolved == resolved.anchor or resolved.parent == resolved


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


def validate_stage_exe_name(name: str) -> None:
    path = Path(name)
    if not name or name in (".", "..") or path.is_absolute():
        raise SystemExit(f"--stage-exe-name must be a plain filename: {name!r}")
    if path.name != name or "/" in name or "\\" in name or ".." in path.parts:
        raise SystemExit(f"--stage-exe-name must be a plain filename: {name!r}")


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
        raise SystemExit(f"path escapes stage dir: {path}")
    if path.is_dir():
        for child in path.rglob("*"):
            if is_link_like(child):
                raise SystemExit(
                    f"refusing symlink/junction/reparse point: {child}")
            if not is_relative_to(child, root_resolved):
                raise SystemExit(f"path escapes stage dir: {child}")


def ensure_stage_dir(stage_dir: Path) -> None:
    if is_root_dir(stage_dir):
        raise SystemExit(f"refusing to use root directory as stage dir: {stage_dir}")
    if stage_dir.exists() and is_link_like(stage_dir):
        raise SystemExit(f"refusing symlink/junction stage dir: {stage_dir}")
    stage_dir.mkdir(parents=True, exist_ok=True)
    marker = stage_dir / MARKER
    children = list(stage_dir.iterdir())
    if not marker.exists():
        if children:
            raise SystemExit(
                f"stage dir is not empty and lacks {MARKER}: {stage_dir}")
        marker.write_text("snesrecomp same-path benchmark stage\n",
                          encoding="utf-8")


def clear_stage_dir(stage_dir: Path) -> None:
    marker = stage_dir / MARKER
    if not marker.exists():
        raise SystemExit(f"refusing to clear unmarked stage dir: {stage_dir}")
    verify_tree_inside(stage_dir, stage_dir)
    for child in stage_dir.iterdir():
        if child == marker:
            continue
        verify_tree_inside(child, stage_dir)
        if child.is_dir():
            shutil.rmtree(child)
        else:
            child.unlink()


def copy_optional_file(src_dir: Path, dst_dir: Path, name: str) -> None:
    src = src_dir / name
    if src.exists():
        validate_no_links_under(src)
        shutil.copy2(src, dst_dir / name)


def copy_payload(source_exe: Path, stage_dir: Path, stage_exe_name: str) -> dict:
    source_dir = source_exe.parent
    ensure_stage_dir(stage_dir)
    clear_stage_dir(stage_dir)

    validate_no_links_under(source_exe)
    shutil.copy2(source_exe, stage_dir / stage_exe_name)
    for dll in sorted(source_dir.glob("*.dll")):
        validate_no_links_under(dll)
        shutil.copy2(dll, stage_dir / dll.name)
    for name in CONFIG_FILES:
        copy_optional_file(source_dir, stage_dir, name)
    for name in PAYLOAD_DIRS:
        src = source_dir / name
        if src.exists():
            validate_no_links_under(src)
            shutil.copytree(src, stage_dir / name)

    manifest = {
        "source_exe": bench.file_meta(source_exe),
        "source_dir": str(source_dir),
        "stage_exe": str(stage_dir / stage_exe_name),
        "source_inputs": {
            name: bench.maybe_file_meta(source_dir / name)
            for name in CONFIG_FILES
        },
        "source_dlls": [
            bench.file_meta(path) for path in sorted(source_dir.glob("*.dll"))
        ],
    }
    (stage_dir / "stage_manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


def maybe_write(path: Path | None, summary: dict) -> None:
    if path:
        path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")


def validate_args(args: argparse.Namespace) -> None:
    if args.frames <= 0 or args.pairs <= 0 or args.warmups < 0:
        raise SystemExit("--frames and --pairs must be positive; --warmups >= 0")
    if args.timeout <= 0.0 or not math.isfinite(args.timeout):
        raise SystemExit("--timeout must be positive and finite")
    if args.a_name == args.b_name:
        raise SystemExit("--a-name and --b-name must be distinct")
    validate_stage_exe_name(args.stage_exe_name)
    if is_root_dir(args.stage_dir):
        raise SystemExit(f"refusing to use root directory as stage dir: {args.stage_dir}")
    for label in ("a_exe", "a_rom", "b_exe", "b_rom"):
        path = getattr(args, label)
        if not path.exists():
            raise SystemExit(f"{label.replace('_', '-')} does not exist: {path}")
        if is_link_like(path):
            raise SystemExit(f"refusing symlink/junction/reparse point: {path}")
    for exe in (args.a_exe, args.b_exe):
        if paths_overlap(exe.parent, args.stage_dir):
            raise SystemExit(
                f"source artifact directory overlaps stage dir: {exe.parent} vs {args.stage_dir}")


def run_staged(args: argparse.Namespace, side: str, name: str, source_exe: Path,
               rom: Path, pair: int | None) -> dict:
    manifest = copy_payload(source_exe, args.stage_dir, args.stage_exe_name)
    rec = bench.run_one(name, args.stage_dir / args.stage_exe_name, rom,
                        args.frames, args.audio, args.stage_dir, args.timeout,
                        side)
    if pair is not None:
        rec["pair"] = pair
    rec["source_artifact"] = manifest
    return rec


def build_summary(args: argparse.Namespace, records: list[dict],
                  deltas: list[float], complete: bool) -> dict:
    staged_exe = args.stage_dir / args.stage_exe_name
    return {
        "complete": complete,
        "control": "same_executable_path",
        "frames": args.frames,
        "pairs": args.pairs,
        "mode": "audio" if args.audio else "throughput",
        "timeout_seconds": args.timeout,
        "stage_dir": str(args.stage_dir),
        "stage_exe": str(staged_exe),
        "environment": bench.relevant_env(),
        "a": args.a_name,
        "b": args.b_name,
        "a_source_exe": bench.file_meta(args.a_exe),
        "a_rom": bench.file_meta(args.a_rom),
        "b_source_exe": bench.file_meta(args.b_exe),
        "b_rom": bench.file_meta(args.b_rom),
        "a_median_fps": bench.median([
            float(r["fps"]) for r in records if r["side"] == "A"
        ]),
        "b_median_fps": bench.median([
            float(r["fps"]) for r in records if r["side"] == "B"
        ]),
        "paired_deltas_percent": deltas,
        "paired_median_delta_percent": bench.median(deltas),
        "records": records,
    }


def main() -> int:
    args = parse_args()
    args.stage_dir = args.stage_dir.resolve()
    args.a_exe = args.a_exe.resolve()
    args.a_rom = args.a_rom.resolve()
    args.b_exe = args.b_exe.resolve()
    args.b_rom = args.b_rom.resolve()
    args.stage_exe_name = args.stage_exe_name or args.a_exe.name
    validate_args(args)
    ensure_stage_dir(args.stage_dir)

    records: list[dict] = []
    for _ in range(args.warmups):
        run_staged(args, "A", args.a_name, args.a_exe, args.a_rom, None)
        run_staged(args, "B", args.b_name, args.b_exe, args.b_rom, None)

    deltas: list[float] = []
    for i in range(args.pairs):
        order = [
            ("A", args.a_name, args.a_exe, args.a_rom),
            ("B", args.b_name, args.b_exe, args.b_rom),
        ]
        if i % 2:
            order.reverse()
        pair_records: dict[str, dict] = {}
        for side, name, exe, rom in order:
            rec = run_staged(args, side, name, exe, rom, i + 1)
            records.append(rec)
            pair_records[side] = rec
            maybe_write(args.json_out,
                        build_summary(args, records, deltas, False))
        a_fps = float(pair_records["A"]["fps"])
        b_fps = float(pair_records["B"]["fps"])
        deltas.append((b_fps - a_fps) * 100.0 / a_fps)
        maybe_write(args.json_out, build_summary(args, records, deltas, False))

    summary = build_summary(args, records, deltas, True)
    print(json.dumps(summary, indent=2))
    maybe_write(args.json_out, summary)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
