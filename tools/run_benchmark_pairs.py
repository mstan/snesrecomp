#!/usr/bin/env python3
"""Run order-balanced snesrecomp benchmark pairs.

This helper consumes the existing title CLI contract:

    <exe> --benchmark <frames> <rom>
    <exe> --benchmark-audio <frames> <rom>

It does not add instrumentation to the runtime. It serializes A/B runs,
captures the machine-readable SNESRECOMP_BENCHMARK line, and reports paired
deltas plus raw records for the performance burn-down.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import re
import statistics
import subprocess
import sys
import time
from pathlib import Path


PREFIX = "SNESRECOMP_BENCHMARK "
CONFIG_FILES = ("config.ini", "config.local.ini", "rom.cfg", "keybinds.ini")
REPORT_FILE = "last_run_report.json"
ENV_PREFIXES = ("SNESRECOMP_",)
ENV_NAMES = (
    "SDL_",
)
BENCH_RENDERER_RE = re.compile(
    r"SNESRECOMP_BENCHMARK_RENDERER\s+name=(?P<name>\S+)\s+vsync=(?P<vsync>-?\d+)"
)


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--frames", type=int, required=True)
    p.add_argument("--pairs", type=int, default=5)
    p.add_argument("--warmups", type=int, default=1)
    p.add_argument("--timeout", type=float, default=120.0,
                   help="Per-run timeout in seconds.")
    p.add_argument("--audio", action="store_true",
                   help="Use --benchmark-audio for uncapped audio smoke runs.")
    p.add_argument("--a-name", default="A")
    p.add_argument("--a-exe", type=Path, required=True)
    p.add_argument("--a-rom", type=Path, required=True)
    p.add_argument("--b-name", default="B")
    p.add_argument("--b-exe", type=Path, required=True)
    p.add_argument("--b-rom", type=Path, required=True)
    p.add_argument("--cwd", type=Path,
                   help="Working directory for both runs; defaults to exe dir.")
    p.add_argument("--json-out", type=Path)
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
    meta = file_meta(path)
    meta["exists"] = True
    return meta


def directory_inputs(cwd: Path, exe: Path) -> dict:
    dirs = {
        "cwd": cwd,
        "exe_dir": exe.parent,
    }
    out = {}
    for label, directory in dirs.items():
        out[label] = {
            name: maybe_file_meta(directory / name)
            for name in CONFIG_FILES
        }
    return out


def relevant_env() -> dict:
    selected = {}
    for key, value in os.environ.items():
        if any(key.startswith(prefix) for prefix in ENV_NAMES) or any(
                key.startswith(prefix) for prefix in ENV_PREFIXES):
            selected[key] = value
    return dict(sorted(selected.items()))


def parse_benchmark_stdout(stdout: str, expected_frames: int | None = None) -> dict:
    record = None
    for line in stdout.splitlines():
        if line.startswith(PREFIX):
            record = json.loads(line[len(PREFIX):])
    if record is None:
        raise ValueError(f"missing {PREFIX!r} line")
    try:
        fps = float(record["fps"])
        raw_frames = record["frames"]
        seconds = float(record["seconds"])
    except (KeyError, TypeError, ValueError) as exc:
        raise ValueError(f"invalid benchmark record: {record!r}") from exc
    if isinstance(raw_frames, bool):
        raise ValueError(f"invalid benchmark frame count: {record!r}")
    if isinstance(raw_frames, float) and not raw_frames.is_integer():
        raise ValueError(f"invalid benchmark frame count: {record!r}")
    try:
        frames = int(raw_frames)
    except (OverflowError, TypeError, ValueError) as exc:
        raise ValueError(f"invalid benchmark frame count: {record!r}") from exc
    if (frames <= 0 or seconds <= 0.0 or fps <= 0.0 or
            not math.isfinite(seconds) or not math.isfinite(fps)):
        raise ValueError(f"non-positive benchmark result: {record!r}")
    if expected_frames is not None and frames != expected_frames:
        raise ValueError(
            f"benchmark frame mismatch: expected {expected_frames}, got {frames}")
    return record


def parse_renderer(stderr: str) -> dict | None:
    for line in stderr.splitlines():
        match = BENCH_RENDERER_RE.search(line)
        if match:
            return {
                "name": match.group("name"),
                "vsync": int(match.group("vsync")),
                "line": line,
            }
    return None


def host_breadcrumbs(stderr: str) -> list[str]:
    keep = []
    needles = (
        "config parsed:",
        "window created:",
        "renderer initialized:",
        "audio disabled",
        "rom resolved:",
        "exe-dir anchor:",
        "widescreen:",
    )
    for line in stderr.splitlines():
        if any(needle in line for needle in needles):
            keep.append(line)
    return keep


def report_snapshot(directory: Path) -> dict:
    path = directory / REPORT_FILE
    meta = maybe_file_meta(path)
    out = {"meta": meta}
    if not path.exists():
        return out
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        out["error"] = str(exc)
        return out
    out["status"] = data.get("status")
    out["wram"] = data.get("wram")
    out["sdl"] = data.get("sdl")
    out["build"] = data.get("build")
    out["reason"] = data.get("reason")
    breadcrumbs = data.get("breadcrumbs")
    if isinstance(breadcrumbs, dict):
        events = breadcrumbs.get("events")
        if isinstance(events, list):
            out["breadcrumbs"] = [
                event for event in events
                if isinstance(event, dict) and isinstance(event.get("msg"), str)
                and any(needle in event["msg"] for needle in (
                    "config parsed:", "window created:", "renderer initialized:",
                    "audio disabled", "rom resolved:", "exe-dir anchor:",
                    "first frame simulated", "widescreen:",
                ))
            ]
    return out


def report_snapshots(cwd: Path, exe: Path) -> dict:
    return {
        "cwd": report_snapshot(cwd),
        "exe_dir": report_snapshot(exe.parent),
    }


def same_file_meta(a: dict, b: dict) -> bool:
    if not a.get("exists") and not b.get("exists"):
        return True
    return (
        a.get("exists") == b.get("exists") and
        a.get("bytes") == b.get("bytes") and
        a.get("mtime_ns") == b.get("mtime_ns") and
        a.get("sha256") == b.get("sha256")
    )


def annotate_report_freshness(before: dict, after: dict) -> dict:
    annotated = {}
    for label, snapshot in after.items():
        out = dict(snapshot)
        before_meta = before.get(label, {}).get("meta", {})
        after_meta = snapshot.get("meta", {})
        out["changed_during_run"] = not same_file_meta(before_meta, after_meta)
        if not after_meta.get("exists"):
            out["freshness"] = "missing"
        elif out["changed_during_run"]:
            out["freshness"] = "fresh"
        else:
            out["freshness"] = "stale_or_unchanged"
        annotated[label] = out
    return annotated


def run_one(name: str, exe: Path, rom: Path, frames: int, audio: bool,
            cwd: Path | None, timeout: float, side: str) -> dict:
    mode = "--benchmark-audio" if audio else "--benchmark"
    run_cwd = cwd or exe.parent
    cmd = [str(exe), mode, str(frames), str(rom)]
    inputs_before = directory_inputs(run_cwd, exe)
    reports_before = report_snapshots(run_cwd, exe)
    start = time.time()
    try:
        cp = subprocess.run(cmd, cwd=str(run_cwd), text=True, timeout=timeout,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    except subprocess.TimeoutExpired as exc:
        raise SystemExit(
            f"{side}/{name} timed out after {timeout:.1f}s: {cmd}"
        ) from exc
    elapsed = time.time() - start
    if cp.returncode != 0:
        sys.stderr.write(cp.stdout)
        sys.stderr.write(cp.stderr)
        raise SystemExit(
            f"{side}/{name} failed with exit code {cp.returncode}: {cmd}")
    try:
        record = parse_benchmark_stdout(cp.stdout, frames)
    except ValueError as exc:
        sys.stderr.write(cp.stdout)
        sys.stderr.write(cp.stderr)
        raise SystemExit(f"{side}/{name}: {exc}") from exc
    record.update({
        "side": side,
        "name": name,
        "exe": str(exe),
        "rom": str(rom),
        "cwd": str(run_cwd),
        "command": cmd,
        "wall_seconds": elapsed,
        "raw_stdout": cp.stdout,
        "raw_stderr": cp.stderr,
        "benchmark_renderer": parse_renderer(cp.stderr),
        "host_breadcrumbs": host_breadcrumbs(cp.stderr),
        "inputs_before": inputs_before,
        "inputs_after": directory_inputs(run_cwd, exe),
        "reports_before": reports_before,
        "reports_after": annotate_report_freshness(
            reports_before, report_snapshots(run_cwd, exe)),
    })
    return record


def median(values: list[float]) -> float:
    return statistics.median(values) if values else 0.0


def build_summary(args: argparse.Namespace, records: list[dict],
                  deltas: list[float], complete: bool) -> dict:
    return {
        "complete": complete,
        "frames": args.frames,
        "pairs": args.pairs,
        "mode": "audio" if args.audio else "throughput",
        "timeout_seconds": args.timeout,
        "cwd": str(args.cwd) if args.cwd else None,
        "effective_cwd_policy": "shared --cwd" if args.cwd else "exe directory per run",
        "environment": relevant_env(),
        "a": args.a_name,
        "b": args.b_name,
        "a_exe": file_meta(args.a_exe),
        "a_rom": file_meta(args.a_rom),
        "b_exe": file_meta(args.b_exe),
        "b_rom": file_meta(args.b_rom),
        "a_median_fps": median([
            float(r["fps"]) for r in records if r["side"] == "A"
        ]),
        "b_median_fps": median([
            float(r["fps"]) for r in records if r["side"] == "B"
        ]),
        "paired_deltas_percent": deltas,
        "paired_median_delta_percent": median(deltas),
        "records": records,
    }


def maybe_write(path: Path | None, summary: dict) -> None:
    if path:
        path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")


def main() -> int:
    args = parse_args()
    if args.frames <= 0 or args.pairs <= 0 or args.warmups < 0:
        raise SystemExit("--frames and --pairs must be positive; --warmups >= 0")
    if args.timeout <= 0.0 or not math.isfinite(args.timeout):
        raise SystemExit("--timeout must be positive and finite")
    if args.a_name == args.b_name:
        raise SystemExit("--a-name and --b-name must be distinct")

    args.a_exe = args.a_exe.resolve()
    args.a_rom = args.a_rom.resolve()
    args.b_exe = args.b_exe.resolve()
    args.b_rom = args.b_rom.resolve()
    if args.cwd:
        args.cwd = args.cwd.resolve()

    for label, path in [
        ("a-exe", args.a_exe), ("a-rom", args.a_rom),
        ("b-exe", args.b_exe), ("b-rom", args.b_rom),
    ]:
        if not path.exists():
            raise SystemExit(f"{label} does not exist: {path}")

    records: list[dict] = []
    for _ in range(args.warmups):
        run_one(args.a_name, args.a_exe, args.a_rom, args.frames,
                args.audio, args.cwd, args.timeout, "A")
        run_one(args.b_name, args.b_exe, args.b_rom, args.frames,
                args.audio, args.cwd, args.timeout, "B")

    deltas: list[float] = []
    for i in range(args.pairs):
        order = [
            ("A", args.a_name, args.a_exe, args.a_rom),
            ("B", args.b_name, args.b_exe, args.b_rom),
        ]
        if i % 2:
            order.reverse()
        pair: dict[str, dict] = {}
        for side, name, exe, rom in order:
            rec = run_one(name, exe, rom, args.frames, args.audio, args.cwd,
                          args.timeout, side)
            rec["pair"] = i + 1
            records.append(rec)
            pair[side] = rec
            maybe_write(args.json_out,
                        build_summary(args, records, deltas, False))
        a_fps = float(pair["A"]["fps"])
        b_fps = float(pair["B"]["fps"])
        deltas.append((b_fps - a_fps) * 100.0 / a_fps)
        maybe_write(args.json_out, build_summary(args, records, deltas, False))

    summary = build_summary(args, records, deltas, True)
    text = json.dumps(summary, indent=2)
    print(text)
    maybe_write(args.json_out, summary)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
