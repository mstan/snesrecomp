//! Native implementation of the LLE-first whole-program analysis pass.
//!
//! This intentionally emits the Python `ProgramManifest` wire format so the
//! two engines can be compared fact-for-fact while the native backend is
//! brought forward.  It does not emit or publish generated C.

#![allow(clippy::type_complexity)]

use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};
use std::fs;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::Instant;

use serde_json::{json, Map, Value};

use snesrecomp_analyzer::cfg::{load_bank_cfg, BankCfg, BankEntry};
use snesrecomp_analyzer::decoder::{
    analyze_function_exit_mx, analyze_function_exit_mx_modes_with_sets, classify_dispatch_helper,
    decode_function, detect_inline_arg_bytes, function_exit_mx_equation,
    function_return_stack_delta_states, DecodeCache, DecodeEnv, FunctionDecodeGraph,
    IndirectDispatchSite,
};
use snesrecomp_analyzer::insn::Mode;
use snesrecomp_analyzer::rom::{
    detect_rom_mapping, load_rom, vector_table_offset, RelocRegion, RomMapping,
};

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
struct VariantKey {
    pc24: u32,
    m: u8,
    x: u8,
}

impl VariantKey {
    fn new(pc24: u32, m: u8, x: u8) -> Self {
        Self {
            pc24: pc24 & 0xFFFFFF,
            m: m & 1,
            x: x & 1,
        }
    }

    fn manifest_key(self) -> String {
        format!("{:06X}:M{}X{}", self.pc24, self.m, self.x)
    }

    fn json(self) -> Value {
        json!({"pc24": self.pc24, "m": self.m, "x": self.x})
    }
}

#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord)]
struct DemandEdge {
    site_pc24: u32,
    kind: &'static str,
    resolution: &'static str,
    target: Option<VariantKey>,
    detail: String,
}

impl DemandEdge {
    fn json(&self) -> Value {
        json!({
            "site_pc24": self.site_pc24,
            "kind": self.kind,
            "resolution": self.resolution,
            "target": self.target.map(VariantKey::json).unwrap_or(Value::Null),
            "detail": self.detail,
        })
    }
}

#[derive(Debug, Clone)]
struct NodeSummary {
    key: VariantKey,
    disposition: &'static str,
    instruction_count: usize,
    min_pc24: u32,
    max_pc24: u32,
    demands: Vec<DemandEdge>,
    reasons: Vec<String>,
}

impl NodeSummary {
    fn json(&self) -> Value {
        json!({
            "key": self.key.json(),
            "disposition": self.disposition,
            "instruction_count": self.instruction_count,
            "min_pc24": self.min_pc24,
            "max_pc24": self.max_pc24,
            "demands": self.demands.iter().map(DemandEdge::json).collect::<Vec<_>>(),
            // Digests are deliberately backend-local cache keys.  The
            // compatibility checker excludes them from semantic comparison.
            "reasons": self.reasons,
            "digest": "native",
        })
    }
}

struct Inputs {
    cfgs: Vec<BankCfg>,
    roots: BTreeSet<VariantKey>,
    entries: HashMap<u32, BankEntry>,
    sibling_entries: HashMap<u32, BTreeSet<u32>>,
    cfg_index: HashMap<u32, usize>,
    data_regions: Vec<(u32, u32, u32)>,
    exclude_ranges: HashMap<u32, Vec<(u32, u32)>>,
    indirect_dispatch: HashMap<u32, IndirectDispatchSite>,
    hle_dispatch: HashMap<u32, String>,
    inline_skip: HashMap<u32, i32>,
    terminal_jsr_sites: BTreeSet<u32>,
    declared_exit_modes: HashMap<(u32, u8, u8), (u8, u8)>,
    /// Synthetic reloc regions redirecting WRAM ram_routine entries to blob
    /// bytes appended to the ROM image (plus any cfg `reloc` directives).
    reloc_regions: Vec<RelocRegion>,
}

fn arg_value(args: &[String], flag: &str) -> Option<String> {
    args.windows(2).find(|w| w[0] == flag).map(|w| w[1].clone())
}

fn arg_values(args: &[String], flag: &str) -> Vec<String> {
    args.windows(2)
        .filter(|w| w[0] == flag)
        .map(|w| w[1].clone())
        .collect()
}

fn has_arg(args: &[String], flag: &str) -> bool {
    args.iter().any(|arg| arg == flag)
}

fn filename_bank(path: &Path) -> Option<u32> {
    let name = path.file_name()?.to_str()?;
    let body = name.strip_prefix("bank")?.strip_suffix(".cfg")?;
    u32::from_str_radix(body, 16).ok()
}

fn mirror_bank(bank: u32) -> Option<u32> {
    let bank = bank & 0xFF;
    if bank < 0x40 || (0x80..0xC0).contains(&bank) {
        Some(bank ^ 0x80)
    } else {
        None
    }
}

fn mirror_pc24(pc24: u32) -> Option<u32> {
    mirror_bank((pc24 >> 16) & 0xFF).map(|bank| (bank << 16) | (pc24 & 0xFFFF))
}

fn rom_u16(rom: &[u8], offset: usize) -> Option<u32> {
    Some(*rom.get(offset)? as u32 | ((*rom.get(offset + 1)? as u32) << 8))
}

fn expand_auto_vectors(cfgs: &mut [BankCfg], rom: &[u8]) {
    let Some(cfg) = cfgs
        .iter_mut()
        .find(|cfg| cfg.bank == 0 && cfg.auto_vectors)
    else {
        return;
    };
    let existing: HashSet<u32> = cfg
        .entries
        .iter()
        .map(|entry| entry.start & 0xFFFF)
        .collect();
    let vector_base = vector_table_offset(rom);
    let vectors = [
        ("I_RESET", rom_u16(rom, vector_base + 0x1C)),
        ("I_NMI", rom_u16(rom, vector_base + 0x0A)),
        ("I_IRQ", rom_u16(rom, vector_base + 0x0E)),
    ];
    for (name, pc) in vectors {
        let Some(pc) = pc else { continue };
        if pc == 0 || pc == 0xFFFF || existing.contains(&pc) {
            continue;
        }
        cfg.entries.push(BankEntry::new(Some(name.to_string()), pc));
    }
}

fn architectural_roots(rom: &[u8]) -> BTreeSet<VariantKey> {
    let mut roots = BTreeSet::new();
    let vector_base = vector_table_offset(rom);
    if let Some(pc) = rom_u16(rom, vector_base + 0x1C) {
        if pc != 0 && pc != 0xFFFF {
            roots.insert(VariantKey::new(pc, 1, 1));
        }
    }
    for offset in [vector_base + 0x0A, vector_base + 0x0E] {
        if let Some(pc) = rom_u16(rom, offset) {
            if pc != 0 && pc != 0xFFFF {
                for m in 0..=1 {
                    for x in 0..=1 {
                        roots.insert(VariantKey::new(pc, m, x));
                    }
                }
            }
        }
    }
    roots
}

fn load_inputs(cfg_dir: &Path, rom: &mut Vec<u8>, all_cfg_roots: bool) -> Result<Inputs, String> {
    let mut paths: Vec<PathBuf> = fs::read_dir(cfg_dir)
        .map_err(|e| format!("{}: {e}", cfg_dir.display()))?
        .filter_map(|entry| entry.ok().map(|entry| entry.path()))
        .filter(|path| filename_bank(path).is_some())
        .collect();
    paths.sort();
    if paths.is_empty() {
        return Err(format!("no bank*.cfg under {}", cfg_dir.display()));
    }

    let mut cfgs = Vec::new();
    for path in paths {
        let bank = filename_bank(&path).unwrap();
        let mut cfg = load_bank_cfg(&path)?;
        cfg.bank = bank as i32;
        cfgs.push(cfg);
    }
    expand_auto_vectors(&mut cfgs, rom.as_slice());

    let mut roots = architectural_roots(rom.as_slice());
    let mut entries = HashMap::new();
    let mut sibling_entries: HashMap<u32, BTreeSet<u32>> = HashMap::new();
    let mut cfg_index = HashMap::new();
    let mut data_regions = Vec::new();
    let mut exclude_ranges = HashMap::new();
    let mut indirect_dispatch = HashMap::new();
    let mut hle_dispatch = HashMap::new();
    let mut inline_skip = HashMap::new();
    let mut terminal_jsr_sites = BTreeSet::new();
    let mut declared_exit_modes = HashMap::new();

    for (index, cfg) in cfgs.iter().enumerate() {
        let bank = cfg.bank as u32 & 0xFF;
        cfg_index.insert(bank, index);
        sibling_entries.insert(bank, cfg.entries.iter().map(|e| e.start & 0xFFFF).collect());
        exclude_ranges.insert(bank, cfg.exclude_ranges.clone());
        if let Some(mirror) = mirror_bank(bank) {
            exclude_ranges
                .entry(mirror)
                .or_insert_with(|| cfg.exclude_ranges.clone());
        }
        for &(region_bank, start, end) in &cfg.data_regions {
            data_regions.push((region_bank & 0xFF, start & 0xFFFF, end & 0xFFFF));
            if let Some(mirror) = mirror_bank(region_bank) {
                data_regions.push((mirror, start & 0xFFFF, end & 0xFFFF));
            }
        }
        for entry in &cfg.entries {
            let pc24 = (bank << 16) | (entry.start & 0xFFFF);
            entries.entry(pc24).or_insert_with(|| entry.clone());
            if all_cfg_roots {
                roots.insert(VariantKey::new(pc24, entry.entry_m, entry.entry_x));
            }
            if let Some(skip) = entry.inline_skip {
                inline_skip.insert(pc24, skip);
                if let Some(mirror) = mirror_pc24(pc24) {
                    inline_skip.insert(mirror, skip);
                }
            }
        }
        for &site_pc16 in &cfg.terminal_jsr {
            let site = (bank << 16) | (site_pc16 & 0xFFFF);
            terminal_jsr_sites.insert(site);
            if let Some(mirror) = mirror_pc24(site) {
                terminal_jsr_sites.insert(mirror);
            }
        }
        for site in &cfg.indirect_dispatch {
            let pc24 = (bank << 16) | (site.site_pc16 & 0xFFFF);
            let value = IndirectDispatchSite {
                count: site.count,
                idx_reg: site.idx_reg,
                table_bases: site.table_bases.clone(),
                ptr_call: site.ptr_call,
                pointer_match: site.pointer_match,
                popped_call_frame: site.popped_call_frame,
                rts_stack: site.rts_stack,
                targets: site.targets.clone(),
            };
            indirect_dispatch.insert(pc24, value.clone());
            if let Some(mirror) = mirror_pc24(pc24) {
                indirect_dispatch.entry(mirror).or_insert(value);
            }
        }
        for (&site, name) in &cfg.hle_dispatch {
            hle_dispatch.insert(site & 0xFFFF, name.clone());
        }
        for &(exit_bank, pc, exit_m, exit_x) in &cfg.exit_mx_at {
            let target = ((exit_bank as u32) << 16) | (pc & 0xFFFF);
            for resolved in [Some(target), mirror_pc24(target)].into_iter().flatten() {
                for m in 0..=1 {
                    for x in 0..=1 {
                        declared_exit_modes.insert((resolved, m, x), (exit_m & 1, exit_x & 1));
                    }
                }
            }
        }
        for &(exit_bank, pc, m, x, exit_m, exit_x) in &cfg.exit_mx_at_per_variant {
            let target = ((exit_bank as u32) << 16) | (pc & 0xFFFF);
            for resolved in [Some(target), mirror_pc24(target)].into_iter().flatten() {
                declared_exit_modes.insert((resolved, m & 1, x & 1), (exit_m & 1, exit_x & 1));
            }
        }
        let mut hle_entries: BTreeSet<u32> = cfg.hle_func.keys().copied().collect();
        hle_entries.extend(cfg.hle_spc_upload.iter().copied());
        for pc in hle_entries {
            let target = (bank << 16) | (pc & 0xFFFF);
            for resolved in [Some(target), mirror_pc24(target)].into_iter().flatten() {
                for m in 0..=1 {
                    for x in 0..=1 {
                        declared_exit_modes
                            .entry((resolved, m, x))
                            .or_insert((m, x));
                    }
                }
            }
        }
    }

    // Reloc regions: any cfg `reloc` directives, plus a synthetic region per
    // `ram_routine` whose blob bytes are appended to the ROM image at a
    // $8000-aligned offset. Redirecting the WRAM entry to a plain LoROM
    // (synth_bank,$8000) target lets the standard offset-based decoder path
    // decode the blob unchanged; the WRAM entry is seeded as a root so its
    // body is materialized even though target_is_code keeps RAM non-code (so
    // callers route through the runtime-guarded dispatch, never a direct call).
    let mut reloc_regions: Vec<RelocRegion> = Vec::new();
    for cfg in &cfgs {
        reloc_regions.extend(cfg.reloc_regions.iter().copied());
    }
    for cfg in &cfgs {
        for rr in &cfg.ram_routines {
            let ram_bank = (rr.pc24 >> 16) & 0xFF;
            let ram_addr = rr.pc24 & 0xFFFF;
            let aligned = (rom.len() + 0x7FFF) & !0x7FFFusize;
            rom.resize(aligned, 0);
            let synth_bank = (aligned / 0x8000) as u32;
            if synth_bank >= 0x80 {
                return Err(format!(
                    "ram_routine {:06X}: synthetic ROM bank ${synth_bank:02X} exceeds LoROM range",
                    rr.pc24
                ));
            }
            rom.extend_from_slice(&rr.bytes);
            // Guard pad (outside the reloc region) so a decoder tail over-read
            // near the terminator can't index past the ROM vec.
            rom.extend_from_slice(&[0u8; 8]);
            reloc_regions.push(RelocRegion::new(
                ram_bank,
                ram_addr,
                synth_bank,
                0x8000,
                rr.bytes.len() as u32,
            ));
            roots.insert(VariantKey::new(rr.pc24, rr.entry_m, rr.entry_x));
        }
    }

    Ok(Inputs {
        cfgs,
        roots,
        entries,
        sibling_entries,
        cfg_index,
        data_regions,
        exclude_ranges,
        indirect_dispatch,
        hle_dispatch,
        inline_skip,
        terminal_jsr_sites,
        declared_exit_modes,
        reloc_regions,
    })
}

fn target_is_code(key: VariantKey, inputs: &Inputs, rom: &[u8]) -> bool {
    let bank = (key.pc24 >> 16) & 0xFF;
    let pc = key.pc24 & 0xFFFF;
    if pc < 0x8000 || (0x40..0x80).contains(&bank) {
        return false;
    }
    let offset = ((bank & 0x7F) as usize) * 0x8000 + (pc as usize - 0x8000);
    if offset >= rom.len() {
        return false;
    }
    if inputs
        .data_regions
        .iter()
        .any(|&(b, start, end)| b == bank && start <= pc && pc < end)
    {
        return false;
    }
    !inputs
        .exclude_ranges
        .get(&bank)
        .into_iter()
        .flatten()
        .any(|&(start, end)| (start & 0xFFFF) <= pc && pc < (end & 0xFFFF))
}

fn target_key(site: u32, raw: u32, kind: Option<&str>, m: u8, x: u8) -> VariantKey {
    let pc24 = if kind == Some("long") || raw > 0xFFFF {
        raw
    } else {
        (site & 0xFF0000) | (raw & 0xFFFF)
    };
    VariantKey::new(pc24, m, x)
}

fn summarize(
    key: VariantKey,
    graph: &FunctionDecodeGraph,
    inputs: &Inputs,
    rom: &[u8],
    poisoned: &HashSet<(u32, u8, u8)>,
    unstable: bool,
) -> NodeSummary {
    let mut edges = BTreeSet::new();
    let mut reasons = BTreeSet::new();
    let mut pcs = Vec::new();
    let mut poison_reasons = BTreeSet::new();

    for decoded in graph.insns() {
        let insn = &decoded.insn;
        let site = insn.addr & 0xFFFFFF;
        pcs.push(site);
        if (insn.mnem == "BRK" || insn.mnem == "COP") && !graph.data_region_exec_pcs.contains(&site)
        {
            poison_reasons.insert(format!("{}_at_{site:06X}", insn.mnem.to_ascii_lowercase()));
        }
        if let Some(entries) = &insn.dispatch_entries {
            if !insn.dispatch_local_goto {
                for &raw in entries {
                    if raw == 0 {
                        continue;
                    }
                    let target = target_key(
                        site,
                        raw,
                        insn.dispatch_kind.as_deref(),
                        insn.m_flag,
                        insn.x_flag,
                    );
                    let resolution = if target_is_code(target, inputs, rom) {
                        "aot_exact"
                    } else {
                        "lle_exact"
                    };
                    edges.insert(DemandEdge {
                        site_pc24: site,
                        kind: "static_dispatch",
                        resolution,
                        target: Some(target),
                        detail: String::new(),
                    });
                }
            }
            continue;
        }
        let direct = if insn.mnem == "JSL" {
            Some((
                "direct_call",
                VariantKey::new(insn.operand, insn.m_flag, insn.x_flag),
            ))
        } else if insn.mnem == "JSR" && insn.mode != Mode::IndirX {
            Some((
                "direct_call",
                VariantKey::new(
                    (site & 0xFF0000) | (insn.operand & 0xFFFF),
                    insn.m_flag,
                    insn.x_flag,
                ),
            ))
        } else if insn.mnem == "JMP" && insn.length == 4 {
            Some((
                "direct_tail_call",
                VariantKey::new(insn.operand, insn.m_flag, insn.x_flag),
            ))
        } else {
            None
        };
        if let Some((kind, target)) = direct {
            let resolution = if target_is_code(target, inputs, rom) {
                "aot_exact"
            } else {
                "lle_exact"
            };
            edges.insert(DemandEdge {
                site_pc24: site,
                kind,
                resolution,
                target: Some(target),
                detail: String::new(),
            });
        }
        if matches!(insn.mnem, "JMP" | "BRA" | "BRL") && insn.length != 4 {
            for successor in &decoded.successors {
                if graph.contains(successor) {
                    continue;
                }
                let target = VariantKey::new(successor.pc, successor.m, successor.x);
                let resolution = if target_is_code(target, inputs, rom) {
                    "aot_exact"
                } else {
                    "lle_exact"
                };
                edges.insert(DemandEdge {
                    site_pc24: site,
                    kind: "direct_tail_call",
                    resolution,
                    target: Some(target),
                    detail: String::new(),
                });
            }
        }
        if insn.dispatch_runtime {
            edges.insert(DemandEdge {
                site_pc24: site,
                kind: "dynamic_dispatch",
                resolution: "lle_dynamic",
                target: None,
                detail: format!("table_base={:04X}", insn.operand & 0xFFFF),
            });
        }
    }

    for &(site, ref target_key) in &graph.boundary_exits {
        let target = VariantKey::new(target_key.pc, target_key.m, target_key.x);
        let resolution = if target_is_code(target, inputs, rom) {
            "aot_exact"
        } else {
            "lle_exact"
        };
        edges.insert(DemandEdge {
            site_pc24: site & 0xFFFFFF,
            kind: "direct_tail_call",
            resolution,
            target: Some(target),
            detail: "declared_boundary".to_string(),
        });
    }

    for item in &graph.unresolved_indirects {
        edges.insert(DemandEdge {
            site_pc24: item.site_pc24,
            kind: "unresolved_indirect",
            resolution: "lle_dynamic",
            target: None,
            detail: format!(
                "{}:mode={}:operand={:06X}",
                item.mnem,
                item.mode.index(),
                item.operand
            ),
        });
    }
    for item in &graph.suppressed_indirect_calls {
        edges.insert(DemandEdge {
            site_pc24: item.site_pc24,
            kind: "suppressed_indirect_call",
            resolution: "lle_dynamic",
            target: None,
            detail: format!("table_base={:04X}", item.table_base),
        });
    }

    if graph.is_empty() {
        reasons.extend(["empty_decode".to_string(), "structural_poison".to_string()]);
    }
    if !poison_reasons.is_empty() {
        reasons.insert("structural_poison".to_string());
        reasons.extend(poison_reasons);
    }
    if !graph.unresolved_indirects.is_empty() {
        reasons.insert("has_lle_indirect_edge".to_string());
    }
    if !graph.suppressed_indirect_calls.is_empty() {
        reasons.insert("has_lle_suppressed_call_edge".to_string());
    }
    let unknown: Vec<_> = graph
        .unknown_callee_exit_sites
        .iter()
        .copied()
        .filter(|&(_, target, m, x)| {
            !poisoned.contains(&(target, m, x))
                && mirror_pc24(target)
                    .map(|p| !poisoned.contains(&(p, m, x)))
                    .unwrap_or(true)
        })
        .collect();
    if !unknown.is_empty() {
        reasons.insert("unproven_callee_exit".to_string());
        for (site, target, m, x) in unknown {
            reasons.insert(format!(
                "unproven_call_at_{site:06X}_to_{target:06X}_m{m}x{x}"
            ));
        }
    }
    if unstable {
        reasons.insert("unstable_exit_fact".to_string());
    }

    let disposition = if reasons.contains("structural_poison")
        || reasons.contains("unproven_callee_exit")
        || reasons.contains("unstable_exit_fact")
    {
        "lle_only"
    } else {
        "aot_eligible"
    };
    let demands = if reasons.contains("structural_poison") {
        Vec::new()
    } else {
        edges.into_iter().collect()
    };
    NodeSummary {
        key,
        disposition,
        instruction_count: graph.len(),
        min_pc24: pcs.iter().copied().min().unwrap_or(key.pc24),
        max_pc24: pcs.iter().copied().max().unwrap_or(key.pc24),
        demands,
        reasons: reasons.into_iter().collect(),
    }
}

fn discover_helpers(
    graph: &FunctionDecodeGraph,
    rom: &[u8],
    mapping: RomMapping,
    known: &HashMap<u32, String>,
) -> HashMap<u32, String> {
    let mut result = HashMap::new();
    for decoded in graph.insns() {
        let insn = &decoded.insn;
        if !(insn.mnem == "JSL" || (insn.mnem == "JMP" && insn.length == 4)) {
            continue;
        }
        let target = insn.operand & 0xFFFFFF;
        if known.contains_key(&target) || result.contains_key(&target) {
            continue;
        }
        if let Some(kind) =
            classify_dispatch_helper(rom, mapping, (target >> 16) & 0xFF, target & 0xFFFF)
        {
            result.insert(target, kind.to_string());
        }
    }
    result
}

fn discover_inline_args(
    graph: &FunctionDecodeGraph,
    rom: &[u8],
    mapping: RomMapping,
    known: &HashMap<u32, i32>,
    probed: &mut HashSet<u32>,
) -> HashMap<u32, i32> {
    let mut result = HashMap::new();
    for decoded in graph.insns() {
        let insn = &decoded.insn;
        let bank = (insn.addr >> 16) & 0xFF;
        let target = if insn.mnem == "JSL" {
            insn.operand & 0xFFFFFF
        } else if insn.mnem == "JSR" && insn.length == 3 {
            (bank << 16) | (insn.operand & 0xFFFF)
        } else {
            continue;
        };
        if known.contains_key(&target) || result.contains_key(&target) || !probed.insert(target) {
            continue;
        }
        let mut counts = BTreeSet::new();
        for (m, x) in [(0, 0), (1, 1)] {
            if let Some(count) =
                detect_inline_arg_bytes(rom, mapping, (target >> 16) & 0xFF, target & 0xFFFF, m, x)
            {
                if count != 0 {
                    counts.insert(count);
                }
            }
        }
        if counts.len() == 1 {
            result.insert(target, i32::from(*counts.first().unwrap()));
        }
    }
    result
}

type ExitDependency = (u32, u8, u8);
type ExitAssumption = (ExitDependency, u8, u8);

#[derive(Debug, Clone, Default)]
struct ExitEquation {
    local_modes: BTreeSet<(u8, u8)>,
    dependencies: BTreeSet<ExitDependency>,
    assumptions: BTreeSet<ExitAssumption>,
}

fn equation_target(
    dependency: ExitDependency,
    tuple_to_key: &HashMap<ExitDependency, VariantKey>,
) -> Option<VariantKey> {
    tuple_to_key.get(&dependency).copied().or_else(|| {
        mirror_pc24(dependency.0).and_then(|pc24| {
            tuple_to_key
                .get(&(pc24, dependency.1, dependency.2))
                .copied()
        })
    })
}

fn known_equation_modes(
    dependency: ExitDependency,
    exact: &HashMap<ExitDependency, (u8, u8)>,
    sets: &HashMap<ExitDependency, Vec<(u8, u8)>>,
    tuple_to_key: &HashMap<ExitDependency, VariantKey>,
    solved: &BTreeMap<VariantKey, BTreeSet<(u8, u8)>>,
) -> Option<BTreeSet<(u8, u8)>> {
    let mirror = mirror_pc24(dependency.0).map(|pc24| (pc24, dependency.1, dependency.2));
    if let Some(&(m, x)) = exact
        .get(&dependency)
        .or_else(|| mirror.and_then(|key| exact.get(&key)))
    {
        return Some(BTreeSet::from([(m & 1, x & 1)]));
    }
    if let Some(modes) = sets
        .get(&dependency)
        .or_else(|| mirror.and_then(|key| sets.get(&key)))
    {
        return Some(modes.iter().map(|&(m, x)| (m & 1, x & 1)).collect());
    }
    equation_target(dependency, tuple_to_key).and_then(|target| solved.get(&target).cloned())
}

fn solve_exit_equation_sccs(
    equations: &BTreeMap<VariantKey, ExitEquation>,
    exact: &HashMap<ExitDependency, (u8, u8)>,
    sets: &HashMap<ExitDependency, Vec<(u8, u8)>>,
) -> BTreeMap<VariantKey, BTreeSet<(u8, u8)>> {
    if equations.is_empty() {
        return BTreeMap::new();
    }

    let mut tuple_to_key: HashMap<ExitDependency, VariantKey> = equations
        .keys()
        .map(|&key| ((key.pc24, key.m, key.x), key))
        .collect();
    for &key in equations.keys() {
        if let Some(mirror) = mirror_pc24(key.pc24) {
            tuple_to_key.entry((mirror, key.m, key.x)).or_insert(key);
        }
    }

    let mut adjacency: BTreeMap<VariantKey, BTreeSet<VariantKey>> = equations
        .keys()
        .map(|&key| (key, BTreeSet::new()))
        .collect();
    let mut mode_adjacency = adjacency.clone();
    for (&key, equation) in equations {
        for &dependency in &equation.dependencies {
            if let Some(target) = equation_target(dependency, &tuple_to_key) {
                adjacency.get_mut(&key).unwrap().insert(target);
                mode_adjacency.get_mut(&key).unwrap().insert(target);
            }
        }
        for &(dependency, _, _) in &equation.assumptions {
            if let Some(target) = equation_target(dependency, &tuple_to_key) {
                adjacency.get_mut(&key).unwrap().insert(target);
            }
        }
    }

    struct Tarjan<'a> {
        adjacency: &'a BTreeMap<VariantKey, BTreeSet<VariantKey>>,
        next_index: usize,
        indices: HashMap<VariantKey, usize>,
        lowlinks: HashMap<VariantKey, usize>,
        stack: Vec<VariantKey>,
        on_stack: HashSet<VariantKey>,
        components: Vec<BTreeSet<VariantKey>>,
    }
    impl Tarjan<'_> {
        fn visit(&mut self, node: VariantKey) {
            let node_index = self.next_index;
            self.next_index += 1;
            self.indices.insert(node, node_index);
            self.lowlinks.insert(node, node_index);
            self.stack.push(node);
            self.on_stack.insert(node);
            let targets: Vec<_> = self.adjacency[&node].iter().copied().collect();
            for target in targets {
                if !self.indices.contains_key(&target) {
                    self.visit(target);
                    let low = self.lowlinks[&node].min(self.lowlinks[&target]);
                    self.lowlinks.insert(node, low);
                } else if self.on_stack.contains(&target) {
                    let low = self.lowlinks[&node].min(self.indices[&target]);
                    self.lowlinks.insert(node, low);
                }
            }
            if self.lowlinks[&node] != self.indices[&node] {
                return;
            }
            let mut component = BTreeSet::new();
            loop {
                let item = self.stack.pop().unwrap();
                self.on_stack.remove(&item);
                component.insert(item);
                if item == node {
                    break;
                }
            }
            self.components.push(component);
        }
    }
    let mut tarjan = Tarjan {
        adjacency: &adjacency,
        next_index: 0,
        indices: HashMap::new(),
        lowlinks: HashMap::new(),
        stack: Vec::new(),
        on_stack: HashSet::new(),
        components: Vec::new(),
    };
    for &key in equations.keys() {
        if !tarjan.indices.contains_key(&key) {
            tarjan.visit(key);
        }
    }

    let mut solved: BTreeMap<VariantKey, BTreeSet<(u8, u8)>> = BTreeMap::new();
    let mut pending = tarjan.components;
    loop {
        let mut next_pending = Vec::new();
        let mut progressed = false;
        for component in pending {
            let mut values: BTreeMap<_, _> = component
                .iter()
                .map(|&key| (key, equations[&key].local_modes.clone()))
                .collect();
            let mut external: BTreeMap<_, BTreeSet<_>> = component
                .iter()
                .map(|&key| (key, BTreeSet::new()))
                .collect();
            let mut complete = true;
            for &key in &component {
                for &dependency in &equations[&key].dependencies {
                    if equation_target(dependency, &tuple_to_key)
                        .is_some_and(|target| component.contains(&target))
                    {
                        continue;
                    }
                    let Some(modes) =
                        known_equation_modes(dependency, exact, sets, &tuple_to_key, &solved)
                    else {
                        complete = false;
                        break;
                    };
                    external.get_mut(&key).unwrap().extend(modes);
                }
                if !complete {
                    break;
                }
                for &(dependency, _, _) in &equations[&key].assumptions {
                    if equation_target(dependency, &tuple_to_key)
                        .is_some_and(|target| component.contains(&target))
                    {
                        continue;
                    }
                    if known_equation_modes(dependency, exact, sets, &tuple_to_key, &solved)
                        .is_none()
                    {
                        complete = false;
                        break;
                    }
                }
                if !complete {
                    break;
                }
            }
            if !complete {
                next_pending.push(component);
                continue;
            }
            for (&key, modes) in &external {
                values.get_mut(&key).unwrap().extend(modes.iter().copied());
            }
            loop {
                let mut changed = false;
                for &key in &component {
                    let targets: Vec<_> = mode_adjacency[&key]
                        .intersection(&component)
                        .copied()
                        .collect();
                    for target in targets {
                        let target_modes = values[&target].clone();
                        let before = values[&key].len();
                        values.get_mut(&key).unwrap().extend(target_modes);
                        changed |= values[&key].len() != before;
                    }
                }
                if !changed {
                    break;
                }
            }
            let assumptions_hold = component.iter().all(|key| {
                equations[key]
                    .assumptions
                    .iter()
                    .all(|&(dependency, m, x)| {
                        let modes = equation_target(dependency, &tuple_to_key)
                            .filter(|target| component.contains(target))
                            .and_then(|target| values.get(&target).cloned())
                            .or_else(|| {
                                known_equation_modes(
                                    dependency,
                                    exact,
                                    sets,
                                    &tuple_to_key,
                                    &solved,
                                )
                            });
                        modes == Some(BTreeSet::from([(m & 1, x & 1)]))
                    })
            });
            if !assumptions_hold {
                continue;
            }
            solved.extend(values);
            progressed = true;
        }
        if !progressed {
            break;
        }
        pending = next_pending;
    }
    solved
}

fn analyze(
    inputs: &Inputs,
    rom: &[u8],
    max_insns: usize,
    max_nodes: usize,
) -> Result<
    (
        BTreeMap<VariantKey, NodeSummary>,
        HashMap<(u32, u8, u8), (u8, u8)>,
        HashMap<(u32, u8, u8), Vec<(u8, u8)>>,
        HashMap<u32, String>,
        HashMap<u32, i32>,
    ),
    String,
> {
    let mapping = detect_rom_mapping(rom);
    let mut active_exact = inputs.declared_exit_modes.clone();
    let mut active_sets: HashMap<(u32, u8, u8), Vec<(u8, u8)>> = HashMap::new();
    let mut unstable_exact = HashSet::new();
    let mut unstable_sets = HashSet::new();
    let mut poisoned = HashSet::new();
    let mut helpers: HashMap<u32, String> = HashMap::new();
    let mut inline_args = inputs.inline_skip.clone();
    let mut inline_arg_probes = HashSet::new();
    let mut cache = DecodeCache::new();
    let mut summary_cache: HashMap<
        VariantKey,
        (
            Arc<FunctionDecodeGraph>,
            Vec<(u32, u32, u8, u8)>,
            bool,
            NodeSummary,
        ),
    > = HashMap::new();
    for _round in 1..=128 {
        let mut pending = inputs.roots.clone();
        let mut nodes = BTreeMap::new();
        let mut round_exact = BTreeMap::new();
        let mut round_sets = BTreeMap::new();
        let mut round_equations = BTreeMap::new();
        let before_poisoned = poisoned.clone();
        let before_helpers = helpers.clone();
        let before_inline_args = inline_args.clone();

        while let Some(key) = pending.pop_first() {
            if nodes.contains_key(&key) {
                continue;
            }
            if nodes.len() >= max_nodes {
                return Err(format!("program analysis exceeded max_nodes={max_nodes}"));
            }
            let bank = (key.pc24 >> 16) & 0xFF;
            let pc = key.pc24 & 0xFFFF;
            let mirror = mirror_bank(bank);
            let cfg = inputs
                .cfg_index
                .get(&bank)
                .or_else(|| mirror.and_then(|m| inputs.cfg_index.get(&m)))
                .map(|&i| &inputs.cfgs[i]);
            let entry = inputs
                .entries
                .get(&key.pc24)
                .or_else(|| mirror_pc24(key.pc24).and_then(|p| inputs.entries.get(&p)));
            let end = entry.and_then(|entry| entry.end);
            let mut siblings = inputs
                .sibling_entries
                .get(&bank)
                .or_else(|| mirror.and_then(|m| inputs.sibling_entries.get(&m)))
                .cloned()
                .unwrap_or_default();
            siblings.remove(&pc);
            let inline_loops = cfg.map(|cfg| &cfg.inline_dispatch_loops);
            let env = DecodeEnv {
                rom_mapping: mapping,
                max_insns: Some(max_insns),
                dispatch_helpers: Some(&helpers),
                indirect_dispatch: Some(&inputs.indirect_dispatch),
                hle_dispatch: Some(&inputs.hle_dispatch),
                data_regions: Some(&inputs.data_regions),
                callee_exit_mx: Some(&active_exact),
                callee_exit_mx_modes: Some(&active_sets),
                sibling_entry_pcs: Some(&siblings),
                callee_inline_skip: Some(&inline_args),
                inline_dispatch_loop_pcs: inline_loops,
                terminal_jsr_sites: Some(&inputs.terminal_jsr_sites),
                global_inline_skip: Some(&inline_args),
                stop_on_unknown_callee_exit: true,
                reloc_regions: Some(&inputs.reloc_regions),
                ..Default::default()
            };
            let decoded = catch_unwind(AssertUnwindSafe(|| {
                cache.get_or_decode_local(rom, bank, pc, key.m, key.x, end, &env)
            }));
            let Ok(mut graph) = decoded else {
                nodes.insert(
                    key,
                    NodeSummary {
                        key,
                        disposition: "lle_only",
                        instruction_count: 0,
                        min_pc24: key.pc24,
                        max_pc24: key.pc24,
                        demands: Vec::new(),
                        reasons: vec!["decode_budget_exhausted".to_string()],
                    },
                );
                continue;
            };
            // A directly self-recursive function cannot obtain its own exit
            // fact from the outer rounds.  Bootstrap the SCC locally from
            // concrete non-recursive return paths; the M/X lattice has only
            // four elements, so six iterations is a conservative bound.
            let self_keys: HashSet<(u32, u8, u8)> = [
                Some((key.pc24, key.m, key.x)),
                mirror_pc24(key.pc24).map(|pc24| (pc24, key.m, key.x)),
            ]
            .into_iter()
            .flatten()
            .collect();
            if !graph.unknown_callee_exit_sites.is_empty()
                && graph
                    .unknown_callee_exit_sites
                    .iter()
                    .all(|&(_, target, m, x)| self_keys.contains(&(target, m, x)))
            {
                let mut overlay_exact = active_exact.clone();
                let mut overlay_sets = active_sets.clone();
                let mut previous: Option<Vec<(u8, u8)>> = None;
                for _ in 0..6 {
                    let Some(mut modes) = analyze_function_exit_mx_modes_with_sets(
                        &graph,
                        Some(&overlay_exact),
                        Some(&overlay_sets),
                    ) else {
                        break;
                    };
                    modes.sort();
                    modes.dedup();
                    if previous.as_ref() == Some(&modes) {
                        break;
                    }
                    previous = Some(modes.clone());
                    for self_key in &self_keys {
                        overlay_exact.remove(self_key);
                        overlay_sets.remove(self_key);
                        if modes.len() == 1 {
                            overlay_exact.insert(*self_key, modes[0]);
                        } else {
                            overlay_sets.insert(*self_key, modes.clone());
                        }
                    }
                    let self_env = DecodeEnv {
                        callee_exit_mx: Some(&overlay_exact),
                        callee_exit_mx_modes: Some(&overlay_sets),
                        ..env.clone()
                    };
                    let candidate = catch_unwind(AssertUnwindSafe(|| {
                        decode_function(rom, bank, pc, key.m, key.x, end, &self_env)
                    }));
                    let Ok(candidate) = candidate else {
                        break;
                    };
                    if candidate
                        .unknown_callee_exit_sites
                        .iter()
                        .any(|&(_, target, m, x)| !self_keys.contains(&(target, m, x)))
                    {
                        break;
                    }
                    graph = Arc::new(candidate);
                }
            }
            let additions = discover_helpers(&graph, rom, mapping, &helpers);
            if !additions.is_empty() {
                helpers.extend(additions);
                cache = DecodeCache::new();
                summary_cache.clear();
                pending.insert(key);
                continue;
            }
            let inline_additions =
                discover_inline_args(&graph, rom, mapping, &inline_args, &mut inline_arg_probes);
            if !inline_additions.is_empty() {
                inline_args.extend(inline_additions);
                cache = DecodeCache::new();
                summary_cache.clear();
                pending.insert(key);
                continue;
            }
            let unstable = unstable_exact.contains(&(key.pc24, key.m, key.x));
            let relevant_unknown: Vec<_> = graph
                .unknown_callee_exit_sites
                .iter()
                .copied()
                .filter(|&(_, target, m, x)| {
                    !poisoned.contains(&(target, m, x))
                        && mirror_pc24(target)
                            .map(|p| !poisoned.contains(&(p, m, x)))
                            .unwrap_or(true)
                })
                .collect();
            let summary = match summary_cache.get(&key) {
                Some((cached_graph, cached_unknown, cached_unstable, summary))
                    if Arc::ptr_eq(cached_graph, &graph)
                        && cached_unknown == &relevant_unknown
                        && *cached_unstable == unstable =>
                {
                    summary.clone()
                }
                _ => {
                    let summary = summarize(key, &graph, inputs, rom, &poisoned, unstable);
                    summary_cache.insert(
                        key,
                        (
                            graph.clone(),
                            relevant_unknown.clone(),
                            unstable,
                            summary.clone(),
                        ),
                    );
                    summary
                }
            };
            if summary
                .reasons
                .iter()
                .any(|reason| reason == "structural_poison")
            {
                poisoned.insert((key.pc24, key.m, key.x));
            }

            let analysis_unknown = !graph.unknown_callee_exit_sites.is_empty();
            let structural_poison = summary
                .reasons
                .iter()
                .any(|reason| reason == "structural_poison");
            let fact_key = (key.pc24, key.m, key.x);
            let graph_has_poison = graph.insns().iter().any(|decoded| {
                matches!(decoded.insn.mnem, "BRK" | "COP")
                    && !graph
                        .data_region_exec_pcs
                        .contains(&(decoded.insn.addr & 0xFFFFFF))
            });
            let graph_has_dynamic_unknown = !graph.unresolved_indirects.is_empty()
                || !graph.suppressed_indirect_calls.is_empty();
            if !structural_poison && !graph_has_poison && !graph_has_dynamic_unknown {
                if !analysis_unknown {
                    let (local_modes, dependencies) = function_exit_mx_equation(&graph);
                    round_equations.insert(
                        key,
                        ExitEquation {
                            local_modes: local_modes.into_iter().collect(),
                            dependencies: dependencies.into_iter().collect(),
                            assumptions: BTreeSet::new(),
                        },
                    );
                } else {
                    let probe_env = DecodeEnv {
                        stop_on_unknown_callee_exit: false,
                        ..env.clone()
                    };
                    let probe = catch_unwind(AssertUnwindSafe(|| {
                        decode_function(rom, bank, pc, key.m, key.x, end, &probe_env)
                    }));
                    if let Ok(probe) = probe {
                        let probe_has_poison = probe.insns().iter().any(|decoded| {
                            matches!(decoded.insn.mnem, "BRK" | "COP")
                                && !probe
                                    .data_region_exec_pcs
                                    .contains(&(decoded.insn.addr & 0xFFFFFF))
                        });
                        if !probe_has_poison {
                            let (local_modes, dependencies) = function_exit_mx_equation(&probe);
                            let assumptions = graph
                                .unknown_callee_exit_sites
                                .iter()
                                .map(|&(_, target, m, x)| {
                                    ((target & 0xFFFFFF, m & 1, x & 1), m & 1, x & 1)
                                })
                                .collect();
                            round_equations.insert(
                                key,
                                ExitEquation {
                                    local_modes: local_modes.into_iter().collect(),
                                    dependencies: dependencies.into_iter().collect(),
                                    assumptions,
                                },
                            );
                        }
                    }
                }
            }
            if !analysis_unknown
                && !structural_poison
                && !unstable_exact.contains(&fact_key)
                && !inputs.declared_exit_modes.contains_key(&fact_key)
            {
                let (m, x) = analyze_function_exit_mx(&graph, Some(&active_exact));
                if let (Some(m), Some(x)) = (m, x) {
                    round_exact.insert(fact_key, (m & 1, x & 1));
                } else if !unstable_sets.contains(&fact_key) {
                    if let Some(mut modes) = analyze_function_exit_mx_modes_with_sets(
                        &graph,
                        Some(&active_exact),
                        Some(&active_sets),
                    ) {
                        modes.sort();
                        modes.dedup();
                        if modes.len() != 1 {
                            round_sets.insert(fact_key, modes);
                        }
                    }
                }
            }
            for edge in &summary.demands {
                if edge.resolution == "aot_exact" {
                    if let Some(target) = edge.target {
                        if !nodes.contains_key(&target) {
                            pending.insert(target);
                        }
                    }
                }
            }
            nodes.insert(key, summary);
        }

        for (key, modes) in solve_exit_equation_sccs(&round_equations, &active_exact, &active_sets)
        {
            let fact_key = (key.pc24, key.m, key.x);
            if inputs.declared_exit_modes.contains_key(&fact_key)
                || unstable_exact.contains(&fact_key)
                || unstable_sets.contains(&fact_key)
            {
                continue;
            }
            if modes.len() == 1 {
                round_exact
                    .entry(fact_key)
                    .or_insert(*modes.first().unwrap());
            } else {
                round_sets
                    .entry(fact_key)
                    .or_insert_with(|| modes.into_iter().collect());
            }
        }

        let mut next_exact = active_exact.clone();
        for (key, pair) in round_exact {
            if unstable_exact.contains(&key) {
                continue;
            }
            match next_exact.get(&key).copied() {
                None => {
                    next_exact.insert(key, pair);
                    active_sets.remove(&key);
                }
                Some(old) if old != pair => {
                    unstable_exact.insert(key);
                    next_exact.remove(&key);
                }
                _ => {}
            }
        }
        let mut next_sets = active_sets.clone();
        for (key, modes) in round_sets {
            if unstable_sets.contains(&key) || inputs.declared_exit_modes.contains_key(&key) {
                continue;
            }
            // New callee facts can expose an additional return path that was
            // truncated in an earlier round. A complete multi-mode proof is
            // therefore stronger than a previously published inferred exact
            // fact; replace the stale singleton instead of letting it prune
            // one of the now-proven continuations forever.
            next_exact.remove(&key);
            match next_sets.get(&key) {
                None => {
                    next_sets.insert(key, modes);
                }
                Some(old) if old != &modes => {
                    unstable_sets.insert(key);
                    next_sets.remove(&key);
                }
                _ => {}
            }
        }

        // LoROM mirror banks execute the same physical bytes with the same
        // decode environment. If one reachable mirror proves multiple exit
        // modes after its counterpart prematurely published a single mode,
        // retain the stronger multi-mode proof for both variants. Python's
        // solver exposes the same invariant; keeping an exact fact on only
        // one mirror would incorrectly prune a caller continuation.
        let proven_sets: Vec<_> = next_sets
            .iter()
            .map(|(&key, modes)| (key, modes.clone()))
            .collect();
        for ((pc24, m, x), modes) in proven_sets {
            let Some(mirror_pc) = mirror_pc24(pc24) else {
                continue;
            };
            let mirror_key = (mirror_pc, m, x);
            let mirror_node = VariantKey::new(mirror_pc, m, x);
            if nodes.contains_key(&mirror_node)
                && !inputs.declared_exit_modes.contains_key(&mirror_key)
            {
                next_exact.remove(&mirror_key);
                next_sets.insert(mirror_key, modes);
            }
        }
        // A later round may expose an unresolved call beyond a continuation
        // that was previously truncated. Any inferred exit fact from that
        // shorter graph is no longer proven; declared cfg/HLE ABI facts remain
        // authoritative and are intentionally retained.
        for (node_key, node) in &nodes {
            let fact_key = (node_key.pc24, node_key.m, node_key.x);
            if !inputs.declared_exit_modes.contains_key(&fact_key)
                && node
                    .reasons
                    .iter()
                    .any(|reason| reason == "unproven_callee_exit" || reason == "structural_poison")
            {
                next_exact.remove(&fact_key);
                next_sets.remove(&fact_key);
            }
        }
        let stable = next_exact == active_exact
            && next_sets == active_sets
            && before_poisoned == poisoned
            && before_helpers == helpers
            && before_inline_args == inline_args;
        active_exact = next_exact;
        active_sets = next_sets;
        if stable {
            if let (Ok(path), Ok(target_text)) = (
                std::env::var("SNESRECOMP_NATIVE_ANALYSIS_SNAPSHOT"),
                std::env::var("SNESRECOMP_NATIVE_ANALYSIS_SNAPSHOT_TARGET"),
            ) {
                if let Ok(target) = parse_root(&target_text) {
                    let solved =
                        solve_exit_equation_sccs(&round_equations, &active_exact, &active_sets);
                    let equation = round_equations.get(&target);
                    let dependencies = equation
                        .map(|equation| {
                            equation
                                .dependencies
                                .iter()
                                .map(|&(pc24, m, x)| {
                                    let key = VariantKey::new(pc24, m, x);
                                    json!({
                                        "key": key.manifest_key(),
                                        "exact": active_exact.get(&(pc24, m, x)),
                                        "set": active_sets.get(&(pc24, m, x)),
                                    })
                                })
                                .collect::<Vec<_>>()
                        })
                        .unwrap_or_default();
                    let snapshot = json!({
                        "target": target.manifest_key(),
                        "equation": equation.map(|equation| json!({
                            "local_modes": equation.local_modes,
                            "dependencies": dependencies,
                            "assumptions": equation.assumptions,
                        })),
                        "active_exact": active_exact.get(&(target.pc24, target.m, target.x)),
                        "active_set": active_sets.get(&(target.pc24, target.m, target.x)),
                        "solver_result": solved.get(&target),
                        "boundary_exits": summary_cache.get(&target).map(
                            |(graph, _, _, _)| graph.boundary_exits.iter()
                                .map(|(site, successor)| json!({
                                    "site": format!("{site:06X}"),
                                    "target": format!("{:06X}", successor.pc & 0xFFFFFF),
                                    "m": successor.m,
                                    "x": successor.x,
                                }))
                                .collect::<Vec<_>>()
                        ),
                        "return_stack_deltas": summary_cache.get(&target).map(
                            |(graph, _, _, _)| function_return_stack_delta_states(graph)
                                .into_iter()
                                .map(|(pc24, states)| (format!("{pc24:06X}"), states))
                                .collect::<BTreeMap<_, _>>()
                        ),
                    });
                    let _ = fs::write(
                        path,
                        serde_json::to_string_pretty(&snapshot).unwrap() + "\n",
                    );
                }
            }
            return Ok((nodes, active_exact, active_sets, helpers, inline_args));
        }
    }
    Err("program analysis failed to converge within 128 rounds".to_string())
}

fn manifest_json(
    inputs: &Inputs,
    nodes: &BTreeMap<VariantKey, NodeSummary>,
    exact: &HashMap<(u32, u8, u8), (u8, u8)>,
    sets: &HashMap<(u32, u8, u8), Vec<(u8, u8)>>,
    helpers: &HashMap<u32, String>,
    inline_args: &HashMap<u32, i32>,
) -> Value {
    let mut node_map = Map::new();
    for (key, node) in nodes {
        node_map.insert(key.manifest_key(), node.json());
    }
    let mut exit_map = Map::new();
    let mut exact_rows: Vec<_> = exact.iter().collect();
    exact_rows.sort_by_key(|(k, _)| **k);
    for (&(pc24, m, x), &(exit_m, exit_x)) in exact_rows {
        exit_map.insert(
            VariantKey::new(pc24, m, x).manifest_key(),
            json!({"m": exit_m, "x": exit_x}),
        );
    }
    let mut set_map = Map::new();
    let mut set_rows: Vec<_> = sets.iter().collect();
    set_rows.sort_by_key(|(k, _)| **k);
    for (&(pc24, m, x), modes) in set_rows {
        set_map.insert(
            VariantKey::new(pc24, m, x).manifest_key(),
            Value::Array(
                modes
                    .iter()
                    .map(|&(m, x)| json!({"m": m, "x": x}))
                    .collect(),
            ),
        );
    }
    json!({
        "format_version": 3,
        "roots": inputs.roots.iter().copied().map(VariantKey::json).collect::<Vec<_>>(),
        "exit_modes": exit_map,
        "exit_mode_sets": set_map,
        "nodes": node_map,
        "native_analysis": {
            "dispatch_helpers": helpers.iter().map(
                |(pc24, kind)| (format!("{pc24:06X}"), kind.clone()),
            ).collect::<BTreeMap<_, _>>(),
            "inline_args": inline_args.iter().map(
                |(pc24, count)| (format!("{pc24:06X}"), *count),
            ).collect::<BTreeMap<_, _>>(),
        },
    })
}

fn parse_root(value: &str) -> Result<VariantKey, String> {
    let parts: Vec<_> = value.split(':').collect();
    if parts.len() != 3 {
        return Err(format!("invalid --root {value:?}; expected PC24:M:X"));
    }
    let pc = parts[0].strip_prefix("0x").unwrap_or(parts[0]);
    let pc24 = u32::from_str_radix(pc, 16).map_err(|_| format!("invalid root PC {pc:?}"))?;
    let m: u8 = parts[1]
        .parse()
        .map_err(|_| format!("invalid root M {:?}", parts[1]))?;
    let x: u8 = parts[2]
        .parse()
        .map_err(|_| format!("invalid root X {:?}", parts[2]))?;
    if m > 1 || x > 1 {
        return Err(format!("root M/X must be 0 or 1: {value:?}"));
    }
    Ok(VariantKey::new(pc24, m, x))
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let rom_path = arg_value(&args, "--rom").expect("--rom required");
    let cfg_dir = arg_value(&args, "--cfg-dir").expect("--cfg-dir required");
    let manifest_path = arg_value(&args, "--manifest").expect("--manifest required");
    let max_insns = arg_value(&args, "--max-insns")
        .map(|value| value.parse::<usize>().expect("invalid --max-insns"))
        .unwrap_or(4096);
    let max_nodes = arg_value(&args, "--max-nodes")
        .map(|value| value.parse::<usize>().expect("invalid --max-nodes"))
        .unwrap_or(100_000);
    let started = Instant::now();
    let mut rom = load_rom(&rom_path).expect("load rom");
    let mut inputs = load_inputs(
        Path::new(&cfg_dir),
        &mut rom,
        has_arg(&args, "--all-cfg-roots"),
    )
    .expect("load cfgs");
    for value in arg_values(&args, "--root") {
        inputs
            .roots
            .insert(parse_root(&value).expect("parse --root"));
    }
    let (nodes, exact, sets, helpers, inline_args) =
        analyze(&inputs, &rom, max_insns, max_nodes).expect("native analysis");
    let manifest = manifest_json(&inputs, &nodes, &exact, &sets, &helpers, &inline_args);
    let text = serde_json::to_string_pretty(&manifest).expect("serialize manifest") + "\n";
    fs::write(&manifest_path, text).expect("write manifest");
    let edge_count: usize = nodes.values().map(|node| node.demands.len()).sum();
    let lle = nodes
        .values()
        .filter(|node| node.disposition == "lle_only")
        .count();
    println!(
        "analysis: {} roots -> {} exact variants, {} edges",
        inputs.roots.len(),
        nodes.len(),
        edge_count
    );
    println!(
        "analysis: {} AOT-eligible, {} LLE-only",
        nodes.len() - lle,
        lle
    );
    println!(
        "analysis: wrote {} in {:.3}s",
        Path::new(&manifest_path).display(),
        started.elapsed().as_secs_f64()
    );
}
