"""Analyze once, then atomically emit exact manifest-selected AOT variants."""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import sys
import time


REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "recompiler"))
sys.path.insert(0, str(REPO / "tools"))

from snes65816 import load_rom  # noqa: E402
from v2.program_emit import discover_host_roots, emit_program  # noqa: E402
from v2_analyze import _load_cfgs, build_manifest  # noqa: E402


def _tree_digest(paths) -> str:
    digest = hashlib.sha256()
    for path in sorted({pathlib.Path(p).resolve() for p in paths}):
        if path.is_dir():
            files = sorted(p for p in path.rglob("*.py") if p.is_file())
        else:
            files = [path]
        for file in files:
            digest.update(str(file.relative_to(REPO)).replace("\\", "/").encode())
            digest.update(b"\0")
            digest.update(file.read_bytes())
            digest.update(b"\0")
    return digest.hexdigest()


def _config_digest(parsed) -> str:
    digest = hashlib.sha256()
    cfg_dirs = set()
    for _bank, path, _cfg in parsed:
        cfg_dirs.add(path.resolve().parent)
        digest.update(path.name.encode())
        digest.update(b"\0")
        digest.update(path.read_bytes())
        digest.update(b"\0")
    for cfg_dir in sorted(cfg_dirs):
        header = cfg_dir / "funcs.h"
        if header.exists():
            digest.update(b"funcs.h\0")
            digest.update(header.read_bytes())
            digest.update(b"\0")
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(
        description="manifest-driven LLE-first v2 generation")
    parser.add_argument("--rom", required=True)
    parser.add_argument("--cfg-dir", required=True)
    parser.add_argument("--out-dir", required=True)
    parser.add_argument("--source-root", action="append", default=[])
    parser.add_argument("--no-host-root-scan", action="store_true")
    parser.add_argument("--no-hle", action="store_true")
    parser.add_argument("--max-insns", type=int, default=4096)
    parser.add_argument("--max-nodes", type=int, default=100_000)
    args = parser.parse_args()

    started = time.perf_counter()
    cfg_dir = pathlib.Path(args.cfg_dir).resolve()
    out_dir = pathlib.Path(args.out_dir).resolve()
    rom = load_rom(args.rom)
    parsed = _load_cfgs(cfg_dir)

    source_roots = [pathlib.Path(p).resolve() for p in args.source_root]
    if not source_roots and not args.no_host_root_scan:
        conventional = cfg_dir.parent / "src"
        if conventional.exists():
            source_roots.append(conventional)
    host_roots = () if args.no_host_root_scan else discover_host_roots(
        parsed, source_roots, excluded_roots=(out_dir,))

    manifest, helpers, inline_args = build_manifest(
        rom, parsed, max_insns=args.max_insns, max_nodes=args.max_nodes,
        additional_roots=host_roots)
    result = emit_program(
        rom=rom,
        parsed=parsed,
        manifest=manifest,
        dispatch_helpers=helpers,
        inline_arg_map=inline_args,
        out_dir=out_dir,
        manifest_text=manifest.to_json(),
        generator_digest=_tree_digest((
            REPO / "recompiler" / "v2", pathlib.Path(__file__).resolve(),
            REPO / "tools" / "v2_analyze.py")),
        config_digest=_config_digest(parsed),
        callee_exit_mx={
            (key.pc24, key.m, key.x): pair
            for key, pair in manifest.exit_modes.items()
        },
        enable_hle=not args.no_hle,
    )
    elapsed = time.perf_counter() - started
    print(
        f"v2_emit: {len(manifest.roots)} roots, {result.emitted_variants} "
        f"exact AOT variants, {result.lle_variants} LLE variants")
    print(
        f"v2_emit: {result.emitted_banks} bank(s) emitted, "
        f"{result.reused_banks} reused in {elapsed:.2f}s")
    print(f"v2_emit: atomically published {out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
