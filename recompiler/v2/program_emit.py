"""Manifest-driven, one-pass AOT materialization.

The manifest is the complete source of reachability truth.  This module never
scans generated C and never manufactures a nearby M/X body.  Nodes selected
for AOT are emitted exactly once; every other known node remains represented
by a NULL dispatch-table slot and therefore executes through LLE.
"""

from __future__ import annotations

import hashlib
import json
import pathlib
import re
from collections import defaultdict
from dataclasses import dataclass
from typing import Iterable, Mapping

from .atomic_output import AtomicOutputDir, write_if_changed
from .codegen import (
    set_force_variant_at,
    set_name_resolver,
    set_rom_size,
    set_trampoline_returns,
    set_valid_variants,
    take_rejected_call_targets,
    take_trampoline_returns,
    take_unresolved_call_targets,
    take_unresolved_goto_targets,
)
from .decoder import (
    clear_decode_cache,
    set_active_inline_arg_map,
    set_decode_cache_enabled,
)
from .emit_bank import BankEntry, emit_bank
from .program_analysis import NodeDisposition, ProgramManifest, VariantKey


CACHE_FORMAT_VERSION = 1
_HOST_CALL_RE = re.compile(r"\b([A-Za-z_]\w*(?:_M[01]X[01])?)\s*\(")
_SUFFIX_RE = re.compile(r"^(.*)_M([01])X([01])$")
_HOST_ALIAS_RE = re.compile(
    r"\bvoid\s+([A-Za-z_]\w*)\s*\(CpuState\s*\*cpu\)\s*;"
    r"[^\n]*?/\*\s*\$([0-9A-Fa-f]{2}):([0-9A-Fa-f]{4})\s+alias\s*\*/")


@dataclass(frozen=True)
class EmissionResult:
    emitted_banks: int
    reused_banks: int
    emitted_variants: int
    lle_variants: int


def _host_aliases(parsed):
    aliases = {}
    cfg_dirs = {pathlib.Path(path).resolve().parent for _bank, path, _cfg in parsed}
    for cfg_dir in sorted(cfg_dirs):
        header = cfg_dir / "funcs.h"
        try:
            source = header.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for name, bank, pc in _HOST_ALIAS_RE.findall(source):
            aliases[name] = (int(bank, 16) << 16) | int(pc, 16)
    return aliases


def _cfg_name_maps(parsed):
    name_for_pc = {}
    canonical_for_pc = {}
    templates_exact = {}
    templates_any = {}
    cfg_by_bank = {}
    host_aliases = _host_aliases(parsed)
    claimed_names = set(host_aliases)
    for name, pc24 in sorted(host_aliases.items()):
        name_for_pc[pc24] = name

    for bank, _path, cfg in parsed:
        cfg_by_bank[bank] = cfg
        for entry in cfg.entries:
            pc24 = (bank << 16) | (entry.start & 0xFFFF)
            templates_exact[(pc24, entry.entry_m & 1,
                             entry.entry_x & 1)] = entry
            templates_any.setdefault(pc24, entry)
            canonical_for_pc.setdefault(
                pc24, (entry.entry_m & 1, entry.entry_x & 1))
            if entry.name and entry.name not in claimed_names:
                name_for_pc.setdefault(pc24, entry.name)
                claimed_names.add(entry.name)
    return (name_for_pc, canonical_for_pc, templates_exact,
            templates_any, cfg_by_bank)


def discover_host_roots(parsed, source_roots: Iterable[pathlib.Path],
                        *, excluded_roots=()) -> tuple[VariantKey, ...]:
    """Infer the ROM entry variants called by handwritten host C.

    Host calls are outside the ROM architecture and therefore cannot be found
    by the decoder.  They are nevertheless discoverable build inputs, not
    per-game semantic hints. Generated output and build trees are excluded.
    """
    name_to_entries = defaultdict(list)
    aliases = _host_aliases(parsed)
    canonical_by_name = {}
    for bank, _path, cfg in parsed:
        for entry in cfg.entries:
            if entry.name:
                canonical_by_name.setdefault(
                    entry.name, (entry.entry_m & 1, entry.entry_x & 1))
                name_to_entries[entry.name].append((
                    (bank << 16) | (entry.start & 0xFFFF),
                    entry.entry_m & 1, entry.entry_x & 1))
    for name, pc24 in aliases.items():
        m, x = canonical_by_name.get(name, (1, 1))
        name_to_entries[name] = [(pc24, m, x)]

    excluded = []
    for root in excluded_roots:
        try:
            excluded.append(pathlib.Path(root).resolve())
        except OSError:
            pass

    roots = set()
    for source_root in source_roots:
        source_root = pathlib.Path(source_root)
        if not source_root.exists():
            continue
        paths = [source_root] if source_root.is_file() else source_root.rglob("*.c")
        for path in paths:
            try:
                resolved = path.resolve()
            except OSError:
                continue
            if any(resolved == root or root in resolved.parents
                   for root in excluded):
                continue
            if any(part.lower() in {"build", ".git", "generated", "gen"}
                   for part in resolved.parts):
                continue
            try:
                source = path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            for called in _HOST_CALL_RE.findall(source):
                suffix = _SUFFIX_RE.fullmatch(called)
                base = suffix.group(1) if suffix else called
                candidates = name_to_entries.get(base, ())
                if suffix:
                    wanted = (int(suffix.group(2)), int(suffix.group(3)))
                    roots.update(VariantKey(pc24, *wanted)
                                 for pc24, _m, _x in candidates)
                else:
                    # An unsuffixed C shim is an architectural host boundary,
                    # not proof of cfg-canonical widths. NMI/IRQ in particular
                    # preserve the interrupted M/X state. Analyze every live
                    # combination; the emitted wrapper dispatches on runtime
                    # mirrors and tiers an absent exact body to LLE.
                    roots.update(
                        VariantKey(pc24, m, x)
                        for pc24, _canonical_m, _canonical_x in candidates
                        for m in (0, 1) for x in (0, 1))
    return tuple(sorted(roots))


def _copy_entry(template, *, name, pc24, m, x) -> BankEntry:
    if template is None:
        return BankEntry(name=name, start=pc24 & 0xFFFF,
                         entry_m=m, entry_x=x)
    return BankEntry(
        name=name,
        start=pc24 & 0xFFFF,
        end=template.end,
        entry_m=m,
        entry_x=x,
        tail_call_pc16=template.tail_call_pc16,
        entry_s_offset=template.entry_s_offset,
    )


def build_emission_entries(manifest: ProgramManifest, parsed,
                           *, enable_hle: bool = True):
    (name_for_pc, canonical_for_pc, templates_exact,
     templates_any, cfg_by_bank) = _cfg_name_maps(parsed)
    entries_by_bank = defaultdict(list)
    emitted = defaultdict(set)

    for key, node in sorted(manifest.nodes.items()):
        bank = (key.pc24 >> 16) & 0xFF
        cfg = cfg_by_bank.get(bank)
        pc16 = key.pc24 & 0xFFFF
        has_hle = bool(enable_hle and cfg is not None and (
            pc16 in getattr(cfg, "hle_func", {}) or
            pc16 in getattr(cfg, "hle_spc_upload", ())))
        if node.disposition != NodeDisposition.AOT_ELIGIBLE and not has_hle:
            continue
        template = templates_exact.get(
            (key.pc24, key.m, key.x), templates_any.get(key.pc24))
        name = name_for_pc.get(
            key.pc24, f"bank_{bank:02X}_{pc16:04X}")
        entries_by_bank[bank].append(_copy_entry(
            template, name=name, pc24=key.pc24, m=key.m, x=key.x))
        emitted[key.pc24].add((key.m, key.x))

    # Every analyzed PC is a known executable entry even when it had no cfg
    # label.  Give it the same deterministic synthetic name used by emission
    # and the dispatch table so cross-boundary branches can resolve to an AOT
    # tail call (or exact LLE fallback) instead of an unresolved-goto trap.
    for key in sorted(manifest.nodes):
        name_for_pc.setdefault(
            key.pc24,
            f"bank_{(key.pc24 >> 16) & 0xFF:02X}_{key.pc24 & 0xFFFF:04X}")

    # Keep each friendly alias bound to its cfg-canonical exact variant even
    # though other exact variants sort lexically before it.
    for bank, entries in entries_by_bank.items():
        entries.sort(key=lambda entry: (
            entry.start & 0xFFFF,
            0 if (entry.entry_m & 1, entry.entry_x & 1) ==
                 canonical_for_pc.get((bank << 16) | (entry.start & 0xFFFF))
                 else 1,
            entry.entry_m & 1, entry.entry_x & 1))
    return entries_by_bank, {
        pc24: frozenset(modes) for pc24, modes in emitted.items()
    }, name_for_pc, cfg_by_bank


def emit_dispatch_table(manifest: ProgramManifest, emitted_variants: Mapping,
                        name_for_pc: Mapping[int, str],
                        inline_arg_map: Mapping[int, int]) -> str:
    known_pcs = sorted({key.pc24 for key in manifest.nodes})

    def base_name(pc24):
        return name_for_pc.get(
            pc24, f"bank_{(pc24 >> 16) & 0xFF:02X}_{pc24 & 0xFFFF:04X}")

    lines = [
        "/* Auto-generated from the authoritative LLE/AOT manifest. */",
        "#include \"cpu_state.h\"",
        "",
    ]
    for pc24 in known_pcs:
        base = base_name(pc24)
        for m, x in sorted(emitted_variants.get(pc24, ())):
            lines.append(f"RecompReturn {base}_M{m}X{x}(CpuState *cpu);")
    lines.extend(["", "const DispatchEntry g_dispatch_table[] = {"])
    if not known_pcs:
        lines.append("    { 0xFFFFFFu, { NULL, NULL, NULL, NULL }, 0 },")
    for pc24 in known_pcs:
        base = base_name(pc24)
        slots = ["NULL"] * 4
        for m, x in emitted_variants.get(pc24, ()):
            slots[(m << 1) | x] = f"{base}_M{m}X{x}"
        inline_n = inline_arg_map.get(
            pc24, inline_arg_map.get(pc24 ^ 0x800000, 0))
        lines.append(
            f"    {{ 0x{pc24:06X}u, {{ {', '.join(slots)} }}, "
            f"{inline_n} }},  /* {base} */")
    lines.extend([
        "};",
        "",
        "const unsigned g_dispatch_table_count =",
        "    (unsigned)(sizeof(g_dispatch_table) / sizeof(g_dispatch_table[0]));",
        "",
    ])
    return "\n".join(lines)


def _stable_hash(value) -> str:
    data = json.dumps(value, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(data).hexdigest()


def _bank_cache_key(bank: int, manifest: ProgramManifest,
                    generator_digest: str, config_digest: str,
                    helpers: Mapping, inline_args: Mapping,
                    enable_hle: bool) -> str:
    nodes = [
        (key.manifest_key, node.digest, node.disposition.value)
        for key, node in sorted(manifest.nodes.items())
        if ((key.pc24 >> 16) & 0xFF) == bank
    ]
    return _stable_hash({
        "format": CACHE_FORMAT_VERSION,
        "bank": bank,
        "nodes": nodes,
        "exit_modes": [
            (key.manifest_key, pair[0] & 1, pair[1] & 1)
            for key, pair in sorted(manifest.exit_modes.items())
        ],
        "generator": generator_digest,
        "config": config_digest,
        "helpers": sorted((int(k), str(v)) for k, v in helpers.items()),
        "inline_args": sorted((int(k), int(v)) for k, v in inline_args.items()),
        "hle": bool(enable_hle),
    })


def emit_program(*, rom: bytes, parsed, manifest: ProgramManifest,
                 dispatch_helpers: Mapping, inline_arg_map: Mapping,
                 out_dir: pathlib.Path, manifest_text: str,
                 generator_digest: str, config_digest: str,
                 callee_exit_mx: Mapping | None = None,
                 callee_exit_mx_modes: Mapping | None = None,
                 enable_hle: bool = True) -> EmissionResult:
    entries_by_bank, emitted, name_for_pc, cfg_by_bank = build_emission_entries(
        manifest, parsed, enable_hle=enable_hle)
    all_banks = sorted(set(cfg_by_bank) | set(entries_by_bank))

    live_cache_path = pathlib.Path(out_dir) / ".snesrecomp-cache.json"
    try:
        old_cache = json.loads(live_cache_path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        old_cache = {}
    old_bank_keys = old_cache.get("banks", {})

    workspace = AtomicOutputDir(pathlib.Path(out_dir))
    staging = workspace.staging
    assert staging is not None
    emitted_banks = 0
    reused_banks = 0
    new_bank_keys = {}
    try:
        clear_decode_cache()
        set_decode_cache_enabled(True)
        set_rom_size(len(rom))
        set_name_resolver(dict(name_for_pc))
        set_force_variant_at({})
        set_valid_variants(emitted, authoritative=True)
        set_trampoline_returns(set())
        set_active_inline_arg_map(dict(inline_arg_map) or None)
        take_unresolved_call_targets()
        take_unresolved_goto_targets()
        take_rejected_call_targets()
        take_trampoline_returns()

        for bank in all_banks:
            cache_key = _bank_cache_key(
                bank, manifest, generator_digest, config_digest,
                dispatch_helpers, inline_arg_map, enable_hle)
            new_bank_keys[f"{bank:02X}"] = cache_key
            path = staging / f"bank{bank:02x}_v2.c"
            if old_bank_keys.get(f"{bank:02X}") == cache_key and path.exists():
                reused_banks += 1
                continue

            cfg = cfg_by_bank.get(bank)
            indirect_dispatch = {}
            if cfg is not None:
                for directive in getattr(cfg, "indirect_dispatch", ()):
                    indirect_dispatch[(bank << 16) |
                                      (directive["site_pc16"] & 0xFFFF)] = directive
            source = emit_bank(
                rom, bank, entries_by_bank.get(bank, []),
                dispatch_helpers=dict(dispatch_helpers) or None,
                indirect_call_tables=(getattr(cfg, "indirect_call_tables", None)
                                      if cfg is not None else None),
                indirect_dispatch=indirect_dispatch or None,
                data_regions=(getattr(cfg, "data_regions", None)
                              if cfg is not None else None),
                exclude_ranges=(getattr(cfg, "exclude_ranges", None)
                                if cfg is not None else None),
                callee_exit_mx=dict(callee_exit_mx or {}),
                callee_exit_mx_modes=dict(callee_exit_mx_modes or {}),
                hle_spc_upload=(getattr(cfg, "hle_spc_upload", None)
                                if enable_hle and cfg is not None else None),
                hle_func=(getattr(cfg, "hle_func", None)
                          if enable_hle and cfg is not None else None),
                hle_dispatch=(getattr(cfg, "hle_dispatch", None)
                              if enable_hle and cfg is not None else None),
                inline_arg_map=dict(inline_arg_map) or None,
                declared_entry_pcs=(
                    {entry.start & 0xFFFF for entry in cfg.entries}
                    if cfg is not None else None),
            )
            write_if_changed(path, source)
            emitted_banks += 1

        planned = {
            *(f"bank{bank:02x}_v2.c" for bank in all_banks),
            "dispatch_v2.c",
            "unresolved_stubs_v2.c",
        }
        # Older game integrations used title-specific generated names such as
        # zelda_00_v2.c. Remove every obsolete generated-v2 translation unit
        # inside the staging tree, never from the live directory in place.
        for stale in staging.glob("*_v2.c"):
            if stale.name not in planned:
                stale.unlink()

        write_if_changed(
            staging / "dispatch_v2.c",
            emit_dispatch_table(manifest, emitted, name_for_pc, inline_arg_map))
        write_if_changed(
            staging / "unresolved_stubs_v2.c",
            "/* Manifest-driven generation: unresolved execution remains LLE. */\n")
        write_if_changed(staging / "program_manifest.json", manifest_text)
        cache = {
            "format_version": CACHE_FORMAT_VERSION,
            "generator_digest": generator_digest,
            "config_digest": config_digest,
            "banks": new_bank_keys,
        }
        write_if_changed(
            staging / ".snesrecomp-cache.json",
            json.dumps(cache, indent=2, sort_keys=True) + "\n")
        workspace.publish()
    finally:
        workspace.cleanup()
        clear_decode_cache()

    emitted_count = sum(len(v) for v in emitted.values())
    return EmissionResult(
        emitted_banks=emitted_banks,
        reused_banks=reused_banks,
        emitted_variants=emitted_count,
        lle_variants=len(manifest.nodes) - emitted_count,
    )
