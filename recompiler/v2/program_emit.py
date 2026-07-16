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
from .translation_units import write_bank_translation_units


CACHE_FORMAT_VERSION = 3
_HOST_CALL_RE = re.compile(r"\b([A-Za-z_]\w*(?:_M[01]X[01])?)\s*\(")
_SUFFIX_RE = re.compile(r"^(.*)_M([01])X([01])$")
_HOST_RUNTIME_DISPATCH_RE = re.compile(
    r"\bcpu_dispatch_(?:call_pc(?:_pushed)?|pc(?:_from|_paired)?)\s*"
    r"\(\s*[^,]+,\s*(0x[0-9A-Fa-f]+|[0-9]+)[uUlL]*")
_HOST_ALIAS_RE = re.compile(
    r"\bvoid\s+([A-Za-z_]\w*)\s*\(CpuState\s*\*cpu\)\s*;"
    r"[^\n]*?/\*\s*\$([0-9A-Fa-f]{2}):([0-9A-Fa-f]{4})\s+alias\s*\*/")
_PROFILE_MX_RE = re.compile(r"M([01])X([01])$")


def _lorom_mirror_pc24(pc24: int):
    bank = (pc24 >> 16) & 0xFF
    if bank < 0x40 or 0x80 <= bank < 0xC0:
        return ((bank ^ 0x80) << 16) | (pc24 & 0xFFFF)
    return None


def _architectural_interrupt_pcs(rom: bytes) -> frozenset[int]:
    """Return native/emulation NMI+IRQ vector entries and LoROM mirrors."""
    if len(rom) < 0x8000:
        return frozenset()
    result = set()
    for offset in (0x0A, 0x0E, 0x1A, 0x1E):
        pc = rom[0x7FE0 + offset] | (rom[0x7FE0 + offset + 1] << 8)
        if pc in (0, 0xFFFF):
            continue
        result.add(pc)
        result.add(0x800000 | pc)
    return frozenset(result)


def _cfg_for_bank(cfg_by_bank, bank: int):
    cfg = cfg_by_bank.get(bank & 0xFF)
    if cfg is not None:
        return cfg
    mirror = (bank & 0xFF) ^ 0x80
    if (bank & 0xFF) < 0x40 or 0x80 <= (bank & 0xFF) < 0xC0:
        return cfg_by_bank.get(mirror)
    return None


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

    # Cross-bank `name <addr> <friendly>` declarations (cfg_loader only
    # auto-promotes IN-bank name decls into cfg.entries; a decl whose
    # address lives in a different bank than the cfg that declares it stays
    # declaration-only, see cfg_loader.load_bank_cfg). These are naming
    # hints only here -- they never manufacture a root or change what the
    # manifest analyzer considers reachable, unlike v1/v2_regen's
    # cross-bank auto-promote into emit entries.
    #
    # The same PC is frequently named from multiple cfgs (a bank-switch
    # wrapper reached via JSL from several callers, each with its own
    # cross-bank `name` line for documentation). Two DISTINCT PCs can also
    # claim the SAME friendly name -- e.g. a `func Foo <bodyPC>` in the
    # owning bank plus a cross-bank `name <wrapperPC> Foo` in a caller's
    # cfg, where Foo is really a PHB/PHK-wrapper around a differently
    # named body (see CLAUDE.md's wrapper-bypass note and v2_regen.py's
    # "Track friendly-name claims GLOBALLY" comment for the historical
    # background: v2_regen promotes cross-bank names into cfg.entries
    # guarded by the identical first-seen-wins rule implemented here).
    # Emitting two C functions under the same symbol is a hard build
    # break (MSVC C2084 same-TU / LNK2005 cross-TU), so claim names
    # globally and deterministically: first PC to claim a name (by bank
    # order, then by declaration order within a cfg) keeps it; any later,
    # DISTINCT PC that would reuse an already-claimed name falls back to
    # the emitter's synthetic bank_<BB>_<AAAA> name instead of colliding.
    for bank, _path, cfg in parsed:
        for nd in cfg.names:
            pc24 = nd.addr_24 & 0xFFFFFF
            if pc24 in name_for_pc:
                continue
            if not nd.name or nd.name in claimed_names:
                continue
            name_for_pc[pc24] = nd.name
            claimed_names.add(nd.name)
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
            for target_text in _HOST_RUNTIME_DISPATCH_RE.findall(source):
                pc24 = int(target_text, 0) & 0xFFFFFF
                roots.update(VariantKey(pc24, m, x)
                             for m in (0, 1) for x in (0, 1))
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


def discover_profile_roots(manifest_paths: Iterable[pathlib.Path],
                           declared_entry_pcs: Iterable[int] = ()) \
        -> tuple[VariantKey, ...]:
    """Load clean runtime-observed targets as optional AOT roots.

    A coverage profile influences only materialization: it never authorizes
    behavior, changes decoding semantics, or removes the LLE fallback. Bailed
    observations are deliberately excluded because they are bug evidence, not
    proof that a target is executable code.

    A clean interpreter landing is also not, by itself, proof of a callable
    function boundary.  Computed returns, inline-argument continuations, and
    indirect jumps can all land in the middle of an enclosing function.  Such
    PCs are valid LLE resume points but acquire a false stack/return ABI if
    emitted as standalone C functions.  Promote only hardware call landings
    (``call_gap``) or targets independently declared as function boundaries.
    """
    declared = {int(pc) & 0xFFFFFF for pc in declared_entry_pcs}
    for pc in tuple(declared):
        mirror = _lorom_mirror_pc24(pc)
        if mirror is not None:
            declared.add(mirror)
    roots = set()
    for path in manifest_paths:
        path = pathlib.Path(path)
        try:
            manifest = json.loads(path.read_text(encoding="utf-8"))
        except OSError as exc:
            raise ValueError(f"cannot read profile manifest {path}: {exc}") \
                from exc
        except json.JSONDecodeError as exc:
            raise ValueError(f"invalid profile manifest {path}: {exc}") \
                from exc
        schema = str(manifest.get("schema", ""))
        if not schema.startswith("snesrecomp tier2 coverage"):
            raise ValueError(
                f"unsupported profile manifest schema {schema!r} in {path}")
        discoveries = manifest.get("discoveries", ())
        if not isinstance(discoveries, list):
            raise ValueError(
                f"profile manifest discoveries must be a list in {path}")
        for item in discoveries:
            if not isinstance(item, dict):
                continue
            try:
                clean_hits = int(item.get("clean_hits", 0))
                bail_hits = int(item.get("bail_hits", 0))
            except (TypeError, ValueError):
                continue
            if clean_hits <= 0 or bail_hits != 0:
                continue
            match = _PROFILE_MX_RE.fullmatch(str(item.get("entry_mx", "")))
            if match is None:
                continue
            try:
                target = int(str(item["target_pc24"]), 0) & 0xFFFFFF
            except (KeyError, TypeError, ValueError):
                continue
            if (str(item.get("site_kind", "")) != "call_gap" and
                    target not in declared):
                continue
            roots.add(VariantKey(
                target, int(match.group(1)), int(match.group(2))))
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
        cfg = _cfg_for_bank(cfg_by_bank, bank)
        pc16 = key.pc24 & 0xFFFF
        has_hle = bool(enable_hle and cfg is not None and (
            pc16 in getattr(cfg, "hle_func", {}) or
            pc16 in getattr(cfg, "hle_spc_upload", ())))
        if node.disposition != NodeDisposition.AOT_ELIGIBLE and not has_hle:
            continue
        # An HLE annotation replaces the architectural boundary, not one
        # decoder specialization of it.  Runtime P.M/P.X can legitimately
        # differ from the mode analysis observed (and the same boundary can
        # be entered through a LoROM execution mirror).  Falling back to LLE
        # for an absent exact slot would execute the ROM body instead of the
        # declared override.  Materialize the tiny HLE shim for every exact
        # M/X combination whenever any live manifest demand reaches it.
        modes = ((0, 0), (0, 1), (1, 0), (1, 1)) if has_hle else (
            (key.m, key.x),)
        for m, x in modes:
            if (m, x) in emitted[key.pc24]:
                continue
            template = templates_exact.get(
                (key.pc24, m, x), templates_any.get(key.pc24))
            mirror_pc24 = _lorom_mirror_pc24(key.pc24)
            if template is None and mirror_pc24 is not None:
                template = templates_exact.get(
                    (mirror_pc24, m, x), templates_any.get(mirror_pc24))
            name = name_for_pc.get(
                key.pc24, f"bank_{bank:02X}_{pc16:04X}")
            entries_by_bank[bank].append(_copy_entry(
                template, name=name, pc24=key.pc24, m=m, x=x))
            emitted[key.pc24].add((m, x))

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
        mirror_bank = bank ^ 0x80
        entries.sort(key=lambda entry: (
            entry.start & 0xFFFF,
            0 if (entry.entry_m & 1, entry.entry_x & 1) ==
                 canonical_for_pc.get(
                     (bank << 16) | (entry.start & 0xFFFF),
                     canonical_for_pc.get(
                         (mirror_bank << 16) | (entry.start & 0xFFFF)))
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
                    enable_hle: bool, host_alias_entries: Mapping,
                    shard_threshold_bytes: int, shard_pc_span: int) -> str:
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
        "host_aliases": sorted(
            (name, int(pc24), sorted((int(m), int(x)) for m, x in modes),
             bool(is_interrupt))
            for name, (pc24, modes, is_interrupt)
            in host_alias_entries.items()
            if ((pc24 >> 16) & 0xFF) == bank
        ),
        "hle": bool(enable_hle),
        "sharding": [int(shard_threshold_bytes), int(shard_pc_span)],
    })


def emit_program(*, rom: bytes, parsed, manifest: ProgramManifest,
                 dispatch_helpers: Mapping, inline_arg_map: Mapping,
                 out_dir: pathlib.Path, manifest_text: str,
                 generator_digest: str, config_digest: str,
                 analysis_input_digest: str,
                 callee_exit_mx: Mapping | None = None,
                 callee_exit_mx_modes: Mapping | None = None,
                 enable_hle: bool = True,
                 shard_threshold_bytes: int = 4 * 1024 * 1024,
                 shard_pc_span: int = 0x0800) -> EmissionResult:
    entries_by_bank, emitted, name_for_pc, cfg_by_bank = build_emission_entries(
        manifest, parsed, enable_hle=enable_hle)
    all_banks = sorted(set(cfg_by_bank) | set(entries_by_bank))
    root_pcs = {key.pc24 for key in manifest.roots}
    interrupt_pcs = _architectural_interrupt_pcs(rom)
    host_alias_entries = {
        name: (pc24, emitted.get(pc24, ()), pc24 in interrupt_pcs)
        for name, pc24 in _host_aliases(parsed).items()
        if pc24 in root_pcs
    }

    live_cache_path = pathlib.Path(out_dir) / ".snesrecomp-cache.json"
    try:
        old_cache = json.loads(live_cache_path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        old_cache = {}
    old_bank_keys = old_cache.get("banks", {})
    old_bank_outputs = old_cache.get("bank_outputs", {})
    old_output_hashes = old_cache.get("outputs", {})

    workspace = AtomicOutputDir(pathlib.Path(out_dir))
    staging = workspace.staging
    assert staging is not None
    emitted_banks = 0
    reused_banks = 0
    new_bank_keys = {}
    new_bank_outputs = {}
    planned_bank_outputs = set()
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
                dispatch_helpers, inline_arg_map, enable_hle,
                host_alias_entries, shard_threshold_bytes, shard_pc_span)
            new_bank_keys[f"{bank:02X}"] = cache_key
            previous_names = old_bank_outputs.get(f"{bank:02X}", ())
            reusable_output = bool(previous_names) and all(
                old_output_hashes.get(name)
                and (staging / name).is_file()
                and hashlib.sha256((staging / name).read_bytes()).hexdigest()
                    == old_output_hashes[name]
                for name in previous_names)
            if (old_bank_keys.get(f"{bank:02X}") == cache_key
                    and reusable_output):
                names = tuple(sorted(previous_names))
                new_bank_outputs[f"{bank:02X}"] = list(names)
                planned_bank_outputs.update(names)
                reused_banks += 1
                continue

            cfg = _cfg_for_bank(cfg_by_bank, bank)
            indirect_dispatch = {}
            if cfg is not None:
                for directive in getattr(cfg, "indirect_dispatch", ()):
                    indirect_dispatch[(bank << 16) |
                                      (directive["site_pc16"] & 0xFFFF)] = directive
            indirect_call_tables = (
                getattr(cfg, "indirect_call_tables", None)
                if cfg is not None else None)
            if indirect_call_tables:
                remapped_tables = dict(indirect_call_tables)
                for site, value in indirect_call_tables.items():
                    site_bank = (site >> 16) & 0xFF
                    if site_bank != bank and site_bank == (bank ^ 0x80):
                        remapped_tables[
                            (bank << 16) | (site & 0xFFFF)] = value
                indirect_call_tables = remapped_tables
            data_regions = (
                list(getattr(cfg, "data_regions", ()) or ())
                if cfg is not None else None)
            if data_regions:
                for region_bank, start, end in tuple(data_regions):
                    if region_bank != bank and region_bank == (bank ^ 0x80):
                        data_regions.append((bank, start, end))
            source = emit_bank(
                rom, bank, entries_by_bank.get(bank, []),
                dispatch_helpers=dict(dispatch_helpers) or None,
                indirect_call_tables=indirect_call_tables,
                indirect_dispatch=indirect_dispatch or None,
                data_regions=data_regions,
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
                host_alias_entries=host_alias_entries,
            )
            symbol_pcs = {
                (entry.name or
                 f"bank_{bank:02X}_{entry.start & 0xFFFF:04X}"):
                    entry.start & 0xFFFF
                for entry in entries_by_bank.get(bank, ())
            }
            symbol_pcs.update({
                name: pc24 & 0xFFFF
                for name, (pc24, _modes, _interrupt)
                in host_alias_entries.items()
                if ((pc24 >> 16) & 0xFF) == bank
            })
            names, _changes = write_bank_translation_units(
                staging, bank, symbol_pcs, source,
                threshold_bytes=max(0, int(shard_threshold_bytes)),
                pc_span=max(0, int(shard_pc_span)))
            new_bank_outputs[f"{bank:02X}"] = list(names)
            planned_bank_outputs.update(names)
            emitted_banks += 1

        planned = {
            *planned_bank_outputs,
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
        # Count analyzed architectural variants, not the additional exact HLE
        # shims synthesized for runtime modes absent from the manifest.  Those
        # shims are required override ABI coverage, not newly discovered AOT
        # work, and must not make the reported LLE count negative.
        emitted_count = sum(
            1 for key in manifest.nodes
            if (key.m, key.x) in emitted.get(key.pc24, ()))
        output_names = sorted(planned | {"program_manifest.json"})
        output_hashes = {
            name: hashlib.sha256((staging / name).read_bytes()).hexdigest()
            for name in output_names
        }
        cache = {
            "format_version": CACHE_FORMAT_VERSION,
            "generator_digest": generator_digest,
            "config_digest": config_digest,
            "analysis_input_digest": analysis_input_digest,
            "banks": new_bank_keys,
            "bank_outputs": new_bank_outputs,
            "outputs": output_hashes,
            "stats": {
                "roots": len(manifest.roots),
                "emitted_variants": emitted_count,
                "lle_variants": len(manifest.nodes) - emitted_count,
                "banks": len(all_banks),
            },
        }
        write_if_changed(
            staging / ".snesrecomp-cache.json",
            json.dumps(cache, indent=2, sort_keys=True) + "\n")
        workspace.publish()
    finally:
        workspace.cleanup()
        clear_decode_cache()

    return EmissionResult(
        emitted_banks=emitted_banks,
        reused_banks=reused_banks,
        emitted_variants=emitted_count,
        lle_variants=len(manifest.nodes) - emitted_count,
    )
