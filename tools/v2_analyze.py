"""Build an analysis-only LLE/AOT demand manifest for a game repository.

Unlike ``v2_regen.py``, this command never emits or publishes generated C.
It decodes each exact demanded variant into a transient graph, immediately
compacts it, and writes one deterministic JSON manifest.  This is the bridge
used to validate the new whole-program model before it becomes the emitter's
source of truth.
"""

from __future__ import annotations

import argparse
from dataclasses import replace
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile


REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "recompiler"))

from snes65816 import load_rom  # noqa: E402
from v2.cfg_loader import load_bank_cfg  # noqa: E402
from v2.decoder import (  # noqa: E402
    analyze_function_exit_mx,
    analyze_function_exit_mx_modes,
    classify_dispatch_helper,
    clear_decode_cache,
    decode_function,
    detect_inline_arg_bytes,
    set_decode_cache_enabled,
)
from v2.program_analysis import (  # noqa: E402
    NodeDisposition,
    ProgramAnalyzer,
    ProgramManifest,
    VariantKey,
)


_BANK_CFG_RE = re.compile(r"bank([0-9a-fA-F]+)\.cfg$")


def native_analyzer_path() -> pathlib.Path:
    """Return the configured/default release native-analyzer executable."""
    configured = os.environ.get("SNESRECOMP_NATIVE_ANALYZER")
    if configured:
        return pathlib.Path(configured).expanduser().resolve()
    executable = ("snesrecomp-analyze.exe" if os.name == "nt"
                  else "snesrecomp-analyze")
    return REPO / "recompiler-rs" / "target" / "release" / executable


def build_manifest_native(*, rom_path, cfg_dir, all_cfg_roots=False,
                          additional_roots=(), executable=None):
    """Run the compiled analyzer and load its stable manifest contract."""
    executable = pathlib.Path(
        executable or native_analyzer_path()).resolve()
    if not executable.is_file():
        raise FileNotFoundError(
            f"native analyzer not built at {executable}; run "
            "`python tools/build_native_analyzer.py` from the snesrecomp "
            "checkout")
    fd, temporary = tempfile.mkstemp(
        prefix="snesrecomp-native-analysis-", suffix=".json")
    os.close(fd)
    command = [
        str(executable),
        "--rom", str(pathlib.Path(rom_path).resolve()),
        "--cfg-dir", str(pathlib.Path(cfg_dir).resolve()),
        "--manifest", temporary,
    ]
    if all_cfg_roots:
        command.append("--all-cfg-roots")
    for key in sorted(set(additional_roots)):
        command.extend(("--root", f"{key.pc24:06X}:{key.m}:{key.x}"))
    try:
        completed = subprocess.run(
            command, text=True, capture_output=True, check=False)
        if completed.returncode:
            detail = completed.stderr.strip() or completed.stdout.strip()
            raise RuntimeError(
                f"native analyzer exited {completed.returncode}: {detail}")
        value = json.loads(pathlib.Path(temporary).read_text(
            encoding="utf-8"))
    finally:
        try:
            os.unlink(temporary)
        except OSError:
            pass
    manifest = ProgramManifest.from_dict(value)
    metadata = value.get("native_analysis", {})
    helpers = {
        int(pc24, 16): str(kind)
        for pc24, kind in metadata.get("dispatch_helpers", {}).items()
    }
    inline_args = {
        int(pc24, 16): int(count)
        for pc24, count in metadata.get("inline_args", {}).items()
    }
    return manifest, helpers, inline_args, completed.stdout.strip()


def _lorom_mirror_bank(bank: int):
    bank &= 0xFF
    if bank < 0x40 or 0x80 <= bank < 0xC0:
        return bank ^ 0x80
    return None


def _lorom_mirror_pc24(pc24: int):
    mirror = _lorom_mirror_bank((pc24 >> 16) & 0xFF)
    if mirror is None:
        return None
    return (mirror << 16) | (pc24 & 0xFFFF)


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
            mirror = _lorom_mirror_pc24(site)
            if mirror is not None:
                result.setdefault(mirror, result[site])
    return result


def _declared_exit_modes(parsed) -> dict:
    """Load explicit facts and the generic HLE boundary contract.

    An HLE overlay is a callable C replacement, unlike a ROM coroutine tail
    that may never return lexically. Unless the cfg function boundary declares
    another exit M/X, that overlay must preserve the entry widths. This is a
    property of the optional HLE ABI, not a claim inferred from ROM bytes.
    """
    result = {}

    def targets_with_mirror(target):
        yield target
        mirror = _lorom_mirror_pc24(target)
        if mirror is not None:
            yield mirror

    for bank_id, _path, cfg in parsed:
        for bank, pc, exit_m, exit_x in cfg.exit_mx_at:
            target = ((bank & 0xFF) << 16) | (pc & 0xFFFF)
            for resolved_target in targets_with_mirror(target):
                for entry_m in (0, 1):
                    for entry_x in (0, 1):
                        result[(resolved_target, entry_m, entry_x)] = (
                            exit_m & 1, exit_x & 1)
        for bank, pc, entry_m, entry_x, exit_m, exit_x in \
                cfg.exit_mx_at_per_variant:
            target = ((bank & 0xFF) << 16) | (pc & 0xFFFF)
            for resolved_target in targets_with_mirror(target):
                result[(resolved_target, entry_m & 1, entry_x & 1)] = (
                    exit_m & 1, exit_x & 1)
        hle_entries = set(getattr(cfg, "hle_func", {}))
        hle_entries.update(getattr(cfg, "hle_spc_upload", ()))
        for pc in hle_entries:
            target = ((bank_id & 0xFF) << 16) | (pc & 0xFFFF)
            for resolved_target in targets_with_mirror(target):
                for entry_m in (0, 1):
                    for entry_x in (0, 1):
                        result.setdefault(
                            (resolved_target, entry_m, entry_x),
                            (entry_m, entry_x))
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
                   all_cfg_roots: bool = False,
                   additional_roots=()):
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
        for region_bank, start, end in cfg.data_regions:
            all_data_regions.append((region_bank, start, end))
            mirror = _lorom_mirror_bank(region_bank)
            if mirror is not None:
                all_data_regions.append((mirror, start, end))
        all_exclude_ranges[bank] = tuple(cfg.exclude_ranges)
        mirror = _lorom_mirror_bank(bank)
        if mirror is not None:
            all_exclude_ranges.setdefault(mirror, tuple(cfg.exclude_ranges))
        for entry in cfg.entries:
            key = VariantKey(
                (bank << 16) | (entry.start & 0xFFFF),
                entry.entry_m, entry.entry_x)
            if all_cfg_roots:
                roots.append(key)
            entries_by_address.setdefault(key.pc24, entry)

    # cfg roots are a UNION with the architectural roots, not a
    # replacement: NMI/IRQ must still be analyzed at all four interrupt-
    # entry widths (auto_vectors only seeds their cfg-canonical variant).
    roots.extend(_architectural_roots(rom))
    roots.extend(additional_roots)

    data_regions = tuple(all_data_regions)
    dispatch_map = _indirect_dispatch_map(parsed)
    declared_exit_modes = _declared_exit_modes(parsed)
    active_exit_modes = dict(declared_exit_modes)
    unstable_exit_modes = set()
    round_exit_modes = {}
    # Proven multi-mode exit sets: (pc24, entry_m, entry_x) -> frozenset of
    # (exit_m, exit_x). Published when every exit path resolves but the
    # paths disagree, so no single exact fact exists. Callers fork their
    # post-call continuation across the proven set (decoder) and dispatch
    # on the live width at runtime (emitter) — exact, never speculative.
    active_exit_mode_sets = {}
    unstable_exit_mode_sets = set()
    round_exit_mode_sets = {}
    # Structurally-poisoned variants refute their own demand width: a
    # wrong-width decode that lands in BRK/COP garbage is proof that real
    # execution never enters that (pc24, m, x) — a console running those
    # bytes would crash. A caller's truncated call to a refuted variant is
    # therefore a DEAD path: it neither blocks the caller's exit proof nor
    # contributes exit modes. (The emitted post-call width switch already
    # sends absent variants to LLE, so the dead case stays defensively
    # covered at runtime.) Grows monotonically across rounds.
    poisoned_variants = set()
    dispatch_helpers = {}
    inline_arg_map = {}
    dispatch_helper_probes = set()
    inline_arg_probes = set()

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
        mirror_bank = _lorom_mirror_bank(bank)
        cfg = cfg_by_bank.get(bank)
        if cfg is None and mirror_bank is not None:
            cfg = cfg_by_bank.get(mirror_bank)
        entry = entries_by_address.get(key.pc24)
        if entry is None:
            mirror_pc24 = _lorom_mirror_pc24(key.pc24)
            if mirror_pc24 is not None:
                entry = entries_by_address.get(mirror_pc24)
        end = entry.end if entry is not None else None
        siblings = sibling_entries.get(bank)
        if siblings is None and mirror_bank is not None:
            siblings = sibling_entries.get(mirror_bank)
        siblings = set(siblings or ()) - {pc}

        indirect_call_tables = (
            getattr(cfg, "indirect_call_tables", None) if cfg else None)
        if indirect_call_tables and mirror_bank is not None:
            mirrored_tables = dict(indirect_call_tables)
            for site, value in indirect_call_tables.items():
                site_bank = (site >> 16) & 0xFF
                if site_bank == mirror_bank:
                    mirrored_tables[(bank << 16) | (site & 0xFFFF)] = value
            indirect_call_tables = mirrored_tables

        kwargs = {
            "end": end,
            "max_insns": max_insns,
            "dispatch_helpers": dispatch_helpers or None,
            "indirect_call_tables": indirect_call_tables,
            "indirect_dispatch": dispatch_map or None,
            "data_regions": data_regions or None,
            "callee_exit_mx": active_exit_modes or None,
            "callee_exit_mx_modes": active_exit_mode_sets or None,
            "sibling_entry_pcs": siblings or None,
            "inline_arg_map": inline_arg_map or None,
            # An unknown callee return width is not evidence that M/X is
            # preserved. Stop the speculative caller continuation at that
            # call; once the callee is proven, a later immutable round
            # decodes the continuation with the architectural exit state.
            "stop_on_unknown_callee_exit": True,
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
                if (target not in dispatch_helpers
                        and target not in dispatch_helper_probes):
                    dispatch_helper_probes.add(target)
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
            if target not in inline_arg_map and target not in inline_arg_probes:
                inline_arg_probes.add(target)
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

        # Self-recursive exit fixpoint. A function whose only unknown callee
        # exit is ITSELF (direct recursion at the same entry variant) can
        # never receive its own fact from the outer rounds — the classic
        # SCC bootstrap. Solve it locally as a least fixpoint from below:
        # start from the non-recursive return paths, feed the resulting
        # exit set back as a provisional self-fact, and re-decode until the
        # set stops growing (the lattice has at most four elements). This
        # is exact — every published mode is witnessed by a real decoded
        # return path — and it unblocks every caller chain above the
        # recursive base (e.g. MMX $84:95E6, which gated 19 callers
        # including the Task0 boot chain).
        self_keys = {(key.pc24, key.m, key.x)}
        mirror_pc24 = _lorom_mirror_pc24(key.pc24)
        if mirror_pc24 is not None:
            self_keys.add((mirror_pc24, key.m, key.x))
        if (graph.unknown_callee_exit_sites and all(
                (t, tm, tx) in self_keys
                for (_s, t, tm, tx) in graph.unknown_callee_exit_sites)):
            overlay_exact = dict(active_exit_modes)
            overlay_sets = dict(active_exit_mode_sets)
            prev_modes = None
            for _ in range(6):
                modes = analyze_function_exit_mx_modes(
                    graph, overlay_exact or None, overlay_sets or None)
                if not modes or modes == prev_modes:
                    break
                prev_modes = modes
                for skey in self_keys:
                    overlay_exact.pop(skey, None)
                    overlay_sets.pop(skey, None)
                    if len(modes) == 1:
                        overlay_exact[skey] = next(iter(modes))
                    else:
                        overlay_sets[skey] = frozenset(modes)
                kwargs_self = dict(kwargs)
                kwargs_self["callee_exit_mx"] = overlay_exact or None
                kwargs_self["callee_exit_mx_modes"] = overlay_sets or None
                candidate = decode_function(
                    rom, bank, pc, key.m, key.x, **kwargs_self)
                if any((t, tm, tx) not in self_keys
                       for (_s, t, tm, tx)
                       in candidate.unknown_callee_exit_sites):
                    break
                graph = candidate

        # Strip truncation records for poison-refuted callee widths: those
        # call paths are dead, so they must not demote this node to
        # LLE_ONLY or block its exit-fact publication. Mutates the graph's
        # list in place so the compact summary sees the filtered view.
        if graph.unknown_callee_exit_sites and poisoned_variants:
            def _refuted(site):
                _s, t, tm, tx = site
                if (t, tm, tx) in poisoned_variants:
                    return True
                t_mirror = _lorom_mirror_pc24(t)
                return (t_mirror is not None
                        and (t_mirror, tm, tx) in poisoned_variants)
            kept = [s for s in graph.unknown_callee_exit_sites
                    if not _refuted(s)]
            if len(kept) != len(graph.unknown_callee_exit_sites):
                graph.unknown_callee_exit_sites[:] = kept
        variant_tuple = (key.pc24, key.m, key.x)
        if variant_tuple in unstable_exit_modes:
            graph.unstable_exit_fact = True
        exit_m, exit_x = analyze_function_exit_mx(
            graph, active_exit_modes or None)
        if (not graph.unknown_callee_exit_sites
                and variant_tuple not in unstable_exit_modes
                and variant_tuple not in declared_exit_modes):
            if exit_m is not None and exit_x is not None:
                round_exit_modes[key] = (exit_m & 1, exit_x & 1)
            elif variant_tuple not in unstable_exit_mode_sets:
                # No single exact exit — publish the proven exit-mode SET
                # instead (multi-path SEP/REP callees). Only complete sets
                # count: analyze_function_exit_mx_modes returns None while
                # any exit path is still unresolved, and a truncated decode
                # (unknown callee exit) never publishes at all.
                modes = analyze_function_exit_mx_modes(
                    graph, active_exit_modes or None,
                    active_exit_mode_sets or None)
                if modes and len(modes) > 1:
                    round_exit_mode_sets[key] = frozenset(
                        (m & 1, x & 1) for (m, x) in modes)
        return graph

    # Graphs are intentionally one-shot: compact summaries, not CFG objects,
    # are the reusable cache unit in the new design.
    set_decode_cache_enabled(False)
    clear_decode_cache()
    try:
        # Callee exit M/X changes how every caller's return continuation is
        # decoded.  Re-derive the reachable exact variants against immutable
        # snapshots until both the exit facts and ROM-derived helper facts are
        # stable.  This is the compact replacement for the legacy global
        # cfg-entry x four-width pre-pass: unreachable functions never enter
        # the solver, and no generated-C feedback participates.
        manifest = None
        while True:
            round_exit_modes = {}
            round_exit_mode_sets = {}
            before_helpers = dict(dispatch_helpers)
            before_inline = dict(inline_arg_map)
            before_poisoned = set(poisoned_variants)
            clear_decode_cache()
            manifest = ProgramAnalyzer(
                decode_variant, max_nodes=max_nodes,
                target_is_code=target_is_code).analyze(roots)
            poisoned_variants.update(
                (node_key.pc24, node_key.m, node_key.x)
                for node_key, node in manifest.nodes.items()
                if "structural_poison" in node.reasons)

            # Exact exit proofs only grow. A caller that lacked a callee fact
            # was truncated at the call, so it could not publish a guess that
            # later needs retracting. This is a finite monotone lattice and
            # therefore converges without an arbitrary game-sized round cap.
            next_exit_modes = dict(active_exit_modes)
            for key, pair in sorted(round_exit_modes.items()):
                fact_key = (key.pc24, key.m, key.x)
                if fact_key in unstable_exit_modes:
                    continue
                previous = next_exit_modes.get(fact_key)
                if previous is None:
                    next_exit_modes[fact_key] = pair
                    # An exact proof supersedes any earlier multi-mode set
                    # for the same variant (a graph reshaped by new callee
                    # facts can sharpen ambiguous -> exact). Never keep both.
                    active_exit_mode_sets.pop(fact_key, None)
                elif previous != pair:
                    # A supposedly proven fact changed. Remove it once and
                    # permanently tier that entry to LLE; callers then stop at
                    # the boundary instead of participating in oscillation.
                    unstable_exit_modes.add(fact_key)
                    next_exit_modes.pop(fact_key, None)
            # Same monotone-lattice treatment for the multi-mode sets: a
            # published set that changes between rounds is demoted once and
            # permanently, so callers stop at that boundary (LLE) instead of
            # oscillating. An exact fact for the same variant always wins —
            # never publish both.
            next_exit_mode_sets = dict(active_exit_mode_sets)
            for key, mode_set in sorted(round_exit_mode_sets.items()):
                fact_key = (key.pc24, key.m, key.x)
                if (fact_key in unstable_exit_mode_sets
                        or fact_key in declared_exit_modes):
                    continue
                # A later callee fact can reveal a return path that was
                # truncated when an inferred singleton was first published.
                # The complete multi-mode proof supersedes that stale exact
                # fact; declared ABI facts remain authoritative above.
                next_exit_modes.pop(fact_key, None)
                previous = next_exit_mode_sets.get(fact_key)
                if previous is None:
                    next_exit_mode_sets[fact_key] = mode_set
                elif previous != mode_set:
                    unstable_exit_mode_sets.add(fact_key)
                    next_exit_mode_sets.pop(fact_key, None)

            # If a later round exposes an unresolved call in a variant, an
            # inferred exit fact retained from an earlier shorter graph is no
            # longer proven. Retract it and let callers stop at the boundary.
            # Declared cfg/HLE ABI facts are independent of ROM decode and stay.
            for node_key, node in manifest.nodes.items():
                fact_key = (node_key.pc24, node_key.m, node_key.x)
                if (fact_key not in declared_exit_modes
                        and "unproven_callee_exit" in node.reasons):
                    next_exit_modes.pop(fact_key, None)
                    next_exit_mode_sets.pop(fact_key, None)
            facts_stable = (
                next_exit_modes == active_exit_modes
                and next_exit_mode_sets == active_exit_mode_sets
                and before_helpers == dispatch_helpers
                and before_inline == inline_arg_map
                and before_poisoned == poisoned_variants)
            active_exit_modes = next_exit_modes
            active_exit_mode_sets = next_exit_mode_sets
            if facts_stable:
                break

        assert manifest is not None
        manifest = replace(
            manifest,
            exit_modes={
                VariantKey(pc24, m, x): pair
                for (pc24, m, x), pair in sorted(active_exit_modes.items())
            },
            exit_mode_sets={
                VariantKey(pc24, m, x): frozenset(mode_set)
                for (pc24, m, x), mode_set
                in sorted(active_exit_mode_sets.items())
            })
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
