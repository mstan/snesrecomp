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
use std::sync::atomic::Ordering;
use std::sync::Arc;
use std::time::Duration;
use std::time::Instant;

use serde_json::{json, Map, Value};

use snesrecomp_analyzer::cfg::{load_bank_cfg, BankCfg, BankEntry};
use snesrecomp_analyzer::decoder::{
    analyze_function_exit_mx, analyze_function_exit_mx_modes_with_sets, classify_dispatch_helper,
    decode_function, DecodeCache, DecodeEnv, FunctionDecodeGraph, IndirectDispatchSite,
};
use snesrecomp_analyzer::insn::Mode;
use snesrecomp_analyzer::rom::load_rom;

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
    declared_exit_modes: HashMap<(u32, u8, u8), (u8, u8)>,
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
    let vectors = [
        ("I_RESET", rom_u16(rom, 0x7FFC)),
        ("I_NMI", rom_u16(rom, 0x7FEA)),
        ("I_IRQ", rom_u16(rom, 0x7FEE)),
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
    if let Some(pc) = rom_u16(rom, 0x7FFC) {
        if pc != 0 && pc != 0xFFFF {
            roots.insert(VariantKey::new(pc, 1, 1));
        }
    }
    for offset in [0x7FEA, 0x7FEE] {
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

fn load_inputs(cfg_dir: &Path, rom: &[u8], all_cfg_roots: bool) -> Result<Inputs, String> {
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
    expand_auto_vectors(&mut cfgs, rom);

    let mut roots = architectural_roots(rom);
    let mut entries = HashMap::new();
    let mut sibling_entries: HashMap<u32, BTreeSet<u32>> = HashMap::new();
    let mut cfg_index = HashMap::new();
    let mut data_regions = Vec::new();
    let mut exclude_ranges = HashMap::new();
    let mut indirect_dispatch = HashMap::new();
    let mut hle_dispatch = HashMap::new();
    let mut inline_skip = HashMap::new();
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
        for site in &cfg.indirect_dispatch {
            let pc24 = (bank << 16) | (site.site_pc16 & 0xFFFF);
            let value = IndirectDispatchSite {
                count: site.count,
                idx_reg: site.idx_reg,
                table_bases: site.table_bases.clone(),
                ptr_call: site.ptr_call,
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
        declared_exit_modes,
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
        if insn.mnem == "BRK" || insn.mnem == "COP" {
            poison_reasons.insert(format!("{}_at_{site:06X}", insn.mnem.to_ascii_lowercase()));
        }
        if let Some(entries) = &insn.dispatch_entries {
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
        if let Some(kind) = classify_dispatch_helper(rom, (target >> 16) & 0xFF, target & 0xFFFF) {
            result.insert(target, kind.to_string());
        }
    }
    result
}

fn analyze(
    inputs: &Inputs,
    rom: &[u8],
) -> Result<
    (
        BTreeMap<VariantKey, NodeSummary>,
        HashMap<(u32, u8, u8), (u8, u8)>,
        HashMap<(u32, u8, u8), Vec<(u8, u8)>>,
        HashMap<u32, String>,
    ),
    String,
> {
    let mut active_exact = inputs.declared_exit_modes.clone();
    let mut active_sets: HashMap<(u32, u8, u8), Vec<(u8, u8)>> = HashMap::new();
    let mut unstable_exact = HashSet::new();
    let mut unstable_sets = HashSet::new();
    let mut poisoned = HashSet::new();
    let mut helpers: HashMap<u32, String> = HashMap::new();
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
    let trace_rounds = std::env::var("SNESRECOMP_NATIVE_ANALYSIS_TRACE")
        .map(|value| !value.is_empty() && value != "0")
        .unwrap_or(false);
    let mut decode_time = Duration::ZERO;
    let mut recursion_time = Duration::ZERO;
    let mut summary_time = Duration::ZERO;
    let mut exit_time = Duration::ZERO;

    for round in 1..=128 {
        let mut pending = inputs.roots.clone();
        let mut nodes = BTreeMap::new();
        let mut round_exact = BTreeMap::new();
        let mut round_sets = BTreeMap::new();
        let before_poisoned = poisoned.clone();
        let before_helpers = helpers.clone();

        while let Some(key) = pending.pop_first() {
            if nodes.contains_key(&key) {
                continue;
            }
            if nodes.len() >= 100_000 {
                return Err("program analysis exceeded max_nodes=100000".to_string());
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
                dispatch_helpers: Some(&helpers),
                indirect_dispatch: Some(&inputs.indirect_dispatch),
                hle_dispatch: Some(&inputs.hle_dispatch),
                data_regions: Some(&inputs.data_regions),
                callee_exit_mx: Some(&active_exact),
                callee_exit_mx_modes: Some(&active_sets),
                sibling_entry_pcs: Some(&siblings),
                callee_inline_skip: Some(&inputs.inline_skip),
                inline_dispatch_loop_pcs: inline_loops,
                global_inline_skip: Some(&inputs.inline_skip),
                stop_on_unknown_callee_exit: true,
                ..Default::default()
            };
            let phase_started = Instant::now();
            let decoded = catch_unwind(AssertUnwindSafe(|| {
                cache.get_or_decode_local(rom, bank, pc, key.m, key.x, end, &env)
            }));
            decode_time += phase_started.elapsed();
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
            let phase_started = Instant::now();
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
            recursion_time += phase_started.elapsed();
            let additions = discover_helpers(&graph, rom, &helpers);
            if !additions.is_empty() {
                helpers.extend(additions);
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
            let phase_started = Instant::now();
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
            summary_time += phase_started.elapsed();
            if summary
                .reasons
                .iter()
                .any(|reason| reason == "structural_poison")
            {
                poisoned.insert((key.pc24, key.m, key.x));
            }

            let unknown = !relevant_unknown.is_empty();
            let fact_key = (key.pc24, key.m, key.x);
            let phase_started = Instant::now();
            if !unknown
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
                        if modes.len() > 1 {
                            round_sets.insert(fact_key, modes);
                        }
                    }
                }
            }
            exit_time += phase_started.elapsed();
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
            if unstable_sets.contains(&key) || next_exact.contains_key(&key) {
                continue;
            }
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
        let stable = next_exact == active_exact
            && next_sets == active_sets
            && before_poisoned == poisoned
            && before_helpers == helpers;
        active_exact = next_exact;
        active_sets = next_sets;
        if trace_rounds {
            eprintln!(
                "native analysis round {round}: {} nodes, {} exact exits, {} mode sets",
                nodes.len(),
                active_exact.len(),
                active_sets.len()
            );
        }
        if stable {
            if trace_rounds {
                eprintln!(
                    "native decode cache: {} hits / {} misses",
                    cache.hits.load(Ordering::Relaxed),
                    cache.misses.load(Ordering::Relaxed)
                );
                eprintln!(
                    "native phases: decode {:.3}s, recursion {:.3}s, summary {:.3}s, exits {:.3}s",
                    decode_time.as_secs_f64(),
                    recursion_time.as_secs_f64(),
                    summary_time.as_secs_f64(),
                    exit_time.as_secs_f64(),
                );
            }
            return Ok((nodes, active_exact, active_sets, helpers));
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
            "inline_args": {},
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
    let started = Instant::now();
    let rom = load_rom(&rom_path).expect("load rom");
    let mut inputs = load_inputs(Path::new(&cfg_dir), &rom, has_arg(&args, "--all-cfg-roots"))
        .expect("load cfgs");
    for value in arg_values(&args, "--root") {
        inputs
            .roots
            .insert(parse_root(&value).expect("parse --root"));
    }
    let (nodes, exact, sets, helpers) = analyze(&inputs, &rom).expect("native analysis");
    let manifest = manifest_json(&inputs, &nodes, &exact, &sets, &helpers);
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
