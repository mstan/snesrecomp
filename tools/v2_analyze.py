"""Build an analysis-only LLE/AOT demand manifest for a game repository.

Unlike ``v2_regen.py``, this command never emits or publishes generated C.
It decodes each exact demanded variant into a transient graph, immediately
compacts it, and writes one deterministic JSON manifest.  This is the bridge
used to validate the new whole-program model before it becomes the emitter's
source of truth.
"""

from __future__ import annotations

import argparse
import os
import pathlib
import re
import sys
import tempfile


REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "recompiler"))

from snes65816 import load_rom  # noqa: E402
from v2.cfg_loader import load_bank_cfg  # noqa: E402
from v2.decoder import (  # noqa: E402
    classify_dispatch_helper,
    clear_decode_cache,
    decode_function,
    detect_inline_arg_bytes,
    set_decode_cache_enabled,
)
from v2.program_analysis import (  # noqa: E402
    NodeDisposition,
    ProgramAnalyzer,
    VariantKey,
)


_BANK_CFG_RE = re.compile(r"bank([0-9a-fA-F]+)\.cfg$")


def _load_cfgs(cfg_dir: pathlib.Path):
    parsed = []
    for path in sorted(cfg_dir.glob("bank*.cfg")):
        match = _BANK_CFG_RE.fullmatch(path.name)
        if match:
            parsed.append((int(match.group(1), 16), path,
                           load_bank_cfg(str(path))))
    if not parsed:
        raise ValueError(f"no bank*.cfg under {cfg_dir}")
    return parsed


def _seed_auto_vectors(parsed, rom: bytes) -> None:
    """Mirror v2_regen's byte-derived reset/NMI/IRQ roots."""
    from v2.emit_bank import BankEntry

    if len(rom) < 0x8000:
        return
    for bank, _path, cfg in parsed:
        if bank != 0 or not cfg.auto_vectors:
            continue
        existing_starts = {entry.start & 0xFFFF for entry in cfg.entries}

        def vector(offset: int) -> int:
            return rom[0x7FE0 + offset] | (rom[0x7FE0 + offset + 1] << 8)

        for name, pc in (
                ("I_RESET", vector(0x1C)),
                ("I_NMI", vector(0x0A)),
                ("I_IRQ", vector(0x0E))):
            if pc in (0, 0xFFFF) or pc in existing_starts:
                continue
            cfg.entries.append(BankEntry(name=name, start=pc))
            existing_starts.add(pc)


def _indirect_dispatch_map(parsed) -> dict:
    result = {}
    for bank, _path, cfg in parsed:
        for directive in cfg.indirect_dispatch:
            site = (bank << 16) | (directive["site_pc16"] & 0xFFFF)
            result[site] = {
                key: value for key, value in directive.items()
                if key != "site_pc16"
            }
    return result


def _declared_exit_modes(parsed) -> dict:
    """Load only explicit facts; inferred exits belong in the solver later."""
    result = {}
    for _bank, _path, cfg in parsed:
        for bank, pc, exit_m, exit_x in cfg.exit_mx_at:
            target = ((bank & 0xFF) << 16) | (pc & 0xFFFF)
            for entry_m in (0, 1):
                for entry_x in (0, 1):
                    result[(target, entry_m, entry_x)] = (
                        exit_m & 1, exit_x & 1)
        for bank, pc, entry_m, entry_x, exit_m, exit_x in \
                cfg.exit_mx_at_per_variant:
            target = ((bank & 0xFF) << 16) | (pc & 0xFFFF)
            result[(target, entry_m & 1, entry_x & 1)] = (
                exit_m & 1, exit_x & 1)
    return result


def _atomic_write(path: pathlib.Path, content: str) -> None:
    path = path.resolve()
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(
        prefix=f".{path.name}.tmp-", dir=str(path.parent), text=True)
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as stream:
            stream.write(content)
        os.replace(temporary, path)
    except BaseException:
        try:
            os.unlink(temporary)
        except OSError:
            pass
        raise


def _architectural_roots(rom: bytes) -> list[VariantKey]:
    """Return reset plus every architecturally possible interrupt width.

    Native NMI/IRQ preserve the interrupted M/X flags, so all four variants
    are real possibilities. Reset enters emulation mode with M=X=1.
    """
    if len(rom) < 0x8000:
        return []

    def vector(offset: int) -> int:
        return rom[0x7FE0 + offset] | (rom[0x7FE0 + offset + 1] << 8)

    roots = []
    reset = vector(0x1C)
    if reset not in (0, 0xFFFF):
        roots.append(VariantKey(reset, 1, 1))
    for pc in (vector(0x0A), vector(0x0E)):
        if pc in (0, 0xFFFF):
            continue
        roots.extend(VariantKey(pc, m, x)
                     for m in (0, 1) for x in (0, 1))
    return roots


def build_manifest(rom: bytes, parsed, *, max_insns: int, max_nodes: int,
                   all_cfg_roots: bool = False):
    _seed_auto_vectors(parsed, rom)
    roots = []
    entries_by_address = {}
    sibling_entries = {}
    cfg_by_bank = {}
    all_data_regions = []
    all_exclude_ranges = {}
    for bank, _path, cfg in parsed:
        cfg_by_bank[bank] = cfg
        sibling_entries[bank] = {
            entry.start & 0xFFFF for entry in cfg.entries}
        all_data_regions.extend(cfg.data_regions)
        all_exclude_ranges[bank] = tuple(cfg.exclude_ranges)
        for entry in cfg.entries:
            key = VariantKey(
                (bank << 16) | (entry.start & 0xFFFF),
                entry.entry_m, entry.entry_x)
            if all_cfg_roots:
                roots.append(key)
            entries_by_address.setdefault(key.pc24, entry)

    if not all_cfg_roots:
        roots = _architectural_roots(rom)

    data_regions = tuple(all_data_regions)
    dispatch_map = _indirect_dispatch_map(parsed)
    declared_exit_modes = _declared_exit_modes(parsed)
    dispatch_helpers = {}
    inline_arg_map = {}

    def target_is_code(key: VariantKey) -> bool:
        bank = (key.pc24 >> 16) & 0xFF
        pc = key.pc24 & 0xFFFF
        if pc < 0x8000 or not (bank < 0x40 or bank >= 0x80):
            return False
        offset = (bank & 0x7F) * 0x8000 + (pc - 0x8000)
        if offset >= len(rom):
            return False
        if any((region_bank & 0xFF) == bank
               and (start & 0xFFFF) <= pc < (end & 0xFFFF)
               for region_bank, start, end in data_regions):
            return False
        if any((start & 0xFFFF) <= pc < (end & 0xFFFF)
               for start, end in all_exclude_ranges.get(bank, ())):
            return False
        return True

    def decode_variant(key: VariantKey):
        nonlocal dispatch_helpers, inline_arg_map
        bank = (key.pc24 >> 16) & 0xFF
        pc = key.pc24 & 0xFFFF
        cfg = cfg_by_bank.get(bank)
        entry = entries_by_address.get(key.pc24)
        end = entry.end if entry is not None else None
        siblings = sibling_entries.get(bank, set()) - {pc}

        kwargs = {
            "end": end,
            "max_insns": max_insns,
            "dispatch_helpers": dispatch_helpers or None,
            "indirect_call_tables": (
                getattr(cfg, "indirect_call_tables", None) if cfg else None),
            "indirect_dispatch": dispatch_map or None,
            "data_regions": data_regions or None,
            "callee_exit_mx": declared_exit_modes or None,
            "sibling_entry_pcs": siblings or None,
            "inline_arg_map": inline_arg_map or None,
        }
        graph = decode_function(rom, bank, pc, key.m, key.x, **kwargs)

        # Dispatch-helper and inline-argument facts are properties of ROM
        # code. Discover them at their first reachable call site, replace the
        # immutable input snapshot, and re-decode this node once with the new
        # facts. No generated-C feedback and no retained speculative CFGs.
        helper_additions = {}
        inline_additions = {}
        for decoded in graph.insns.values():
            insn = decoded.insn
            if insn.mnem == "JSL" or (
                    insn.mnem == "JMP" and insn.length == 4):
                target = insn.operand & 0xFFFFFF
                if target not in dispatch_helpers:
                    try:
                        kind = classify_dispatch_helper(
                            rom, (target >> 16) & 0xFF, target & 0xFFFF)
                    except (AssertionError, IndexError):
                        kind = None
                    if kind:
                        helper_additions[target] = kind
            if insn.mnem == "JSL":
                target = insn.operand & 0xFFFFFF
            elif insn.mnem == "JSR" and insn.length == 3:
                target = (bank << 16) | (insn.operand & 0xFFFF)
            else:
                continue
            if target not in inline_arg_map:
                byte_counts = set()
                for probe_m, probe_x in ((0, 0), (1, 1)):
                    try:
                        count = detect_inline_arg_bytes(
                            rom, (target >> 16) & 0xFF,
                            target & 0xFFFF, probe_m, probe_x)
                    except (AssertionError, IndexError):
                        count = None
                    if count:
                        byte_counts.add(count)
                if len(byte_counts) == 1:
                    inline_additions[target] = byte_counts.pop()

        if helper_additions or inline_additions:
            dispatch_helpers = {**dispatch_helpers, **helper_additions}
            inline_arg_map = {**inline_arg_map, **inline_additions}
            kwargs["dispatch_helpers"] = dispatch_helpers or None
            kwargs["inline_arg_map"] = inline_arg_map or None
            graph = decode_function(rom, bank, pc, key.m, key.x, **kwargs)
        return graph

    # Graphs are intentionally one-shot: compact summaries, not CFG objects,
    # are the reusable cache unit in the new design.
    set_decode_cache_enabled(False)
    clear_decode_cache()
    try:
        manifest = ProgramAnalyzer(
            decode_variant, max_nodes=max_nodes,
            target_is_code=target_is_code).analyze(roots)
    finally:
        clear_decode_cache()
    return manifest, dispatch_helpers, inline_arg_map


def main() -> int:
    parser = argparse.ArgumentParser(
        description="build a compact LLE-first program-analysis manifest")
    parser.add_argument("--rom", required=True)
    parser.add_argument("--cfg-dir", required=True)
    parser.add_argument("--manifest", required=True)
    parser.add_argument("--max-insns", type=int, default=4096)
    parser.add_argument("--max-nodes", type=int, default=100_000)
    parser.add_argument(
        "--all-cfg-roots", action="store_true",
        help="migration diagnostic: treat every func declaration as a root; "
             "the default correctly treats func as a boundary only")
    args = parser.parse_args()

    rom = load_rom(args.rom)
    parsed = _load_cfgs(pathlib.Path(args.cfg_dir))
    manifest, helpers, inline_args = build_manifest(
        rom, parsed, max_insns=args.max_insns, max_nodes=args.max_nodes,
        all_cfg_roots=args.all_cfg_roots)
    _atomic_write(pathlib.Path(args.manifest), manifest.to_json())

    counts = {disposition: 0 for disposition in NodeDisposition}
    edge_count = 0
    for node in manifest.nodes.values():
        counts[node.disposition] += 1
        edge_count += len(node.demands)
    print(
        f"analysis: {len(manifest.roots)} roots -> {len(manifest.nodes)} "
        f"exact variants, {edge_count} edges")
    print(
        f"analysis: {counts[NodeDisposition.AOT_ELIGIBLE]} AOT-eligible, "
        f"{counts[NodeDisposition.LLE_ONLY]} LLE-only; "
        f"discovered {len(helpers)} dispatch helpers and "
        f"{len(inline_args)} inline-argument routines")
    print(f"analysis: wrote {pathlib.Path(args.manifest).resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
