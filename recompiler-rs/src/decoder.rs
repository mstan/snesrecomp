//! 65816 function decoder (port of `recompiler/v2/decoder.py`).
//!
//! Decodes a function starting at (bank, start) with an entry (m, x) state into
//! a `FunctionDecodeGraph` — a worklist walk over `DecodeKey`s where the same PC
//! reached under divergent (m, x) produces multiple keyed instances.
//!
//! Unlike the Python, decode-affecting inputs are bundled in one immutable
//! `DecodeEnv` threaded by `&` (no process-global registries). The dependency-
//! keyed decode cache is a Phase 5 concern; correctness lives in the uncached
//! walk.

use std::collections::{HashMap, HashSet};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};

use crate::insn::{decode_insn, Insn, Mode};
use crate::rom::{
    addr_in_reloc_region, addr_to_rom_offset, is_rom_address, RelocRegion, RomMapping,
};

pub const PHP_STACK_MAX_DEPTH: usize = 8;

/// Mnemonics with no fall-through successor.
pub fn is_terminator(mnem: &str) -> bool {
    matches!(mnem, "RTS" | "RTL" | "RTI" | "STP" | "WAI" | "BRK")
}

/// Conditional-branch mnemonics.
pub fn is_cond_branch(mnem: &str) -> bool {
    matches!(
        mnem,
        "BPL" | "BMI" | "BVC" | "BVS" | "BCC" | "BCS" | "BNE" | "BEQ"
    )
}

/// 24-bit address from (bank, pc).
#[inline]
pub fn addr24(bank: u32, pc: u32) -> u32 {
    ((bank & 0xFF) << 16) | (pc & 0xFFFF)
}

/// Identifies a decoded instruction by 24-bit address + entry M/X + PHP/PLP
/// stack history. Two keys are equal iff (pc, m, x, p_stack) all match.
#[derive(Debug, Clone, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct DecodeKey {
    pub pc: u32, // 24-bit ((bank << 16) | local_pc)
    pub m: u8,
    pub x: u8,
    pub p_stack: Vec<(u8, u8)>, // PHP-pushed (m, x) LIFO
}

impl DecodeKey {
    pub fn new(pc: u32, m: u8, x: u8) -> Self {
        DecodeKey {
            pc,
            m,
            x,
            p_stack: Vec::new(),
        }
    }
    pub fn with_stack(pc: u32, m: u8, x: u8, p_stack: Vec<(u8, u8)>) -> Self {
        DecodeKey { pc, m, x, p_stack }
    }
}

/// One instruction decoded at one specific (pc, m, x) entry state.
#[derive(Debug, Clone)]
pub struct DecodedInsn {
    pub key: DecodeKey,
    pub insn: Insn, // m_flag/x_flag set to entry m/x
    pub successors: Vec<DecodeKey>,
}

/// JSR (abs,X) site whose fall-through edge was severed (no cfg dispatch table).
#[derive(Debug, Clone)]
pub struct SuppressedIndirectCall {
    pub site_pc24: u32,
    pub table_base: u32,
    pub function_entry_pc24: u32,
    pub entry_m: u8,
    pub entry_x: u8,
}

/// Dispatch-table entry the decoder refused because the target lands in a
/// cfg `data_region`.
#[derive(Debug, Clone)]
pub struct DispatchTargetSuppressed {
    pub site_pc24: u32,
    pub target_pc24: u32,
    pub reason: String, // 'data_region' (extensible)
    pub table_index: u32,
}

/// Indirect JMP/JML/JSR whose static target list could not be recovered.
/// v2_regen hard-fails on any non-empty list (no-stub policy).
#[derive(Debug, Clone)]
pub struct UnresolvedIndirect {
    pub site_pc24: u32,
    pub mnem: String, // 'JMP' | 'JML' | 'JSR'
    pub mode: Mode,
    pub operand: u32,
    pub function_entry_pc24: u32,
    pub entry_m: u8,
    pub entry_x: u8,
}

/// BEQ/BNE rewritten to an unconditional Goto by the constant-Z fold pass.
#[derive(Debug, Clone)]
pub struct ConstZFold {
    pub branch_pc24: u32,
    pub prev_pc24: u32,
    pub branch_mnem: String,
    pub prev_mnem: String,
    pub prev_imm: u32,
    pub width_bits: u8,
    pub z_value: u8,
    pub taken_kind: String, // 'jump' | 'fall'
    pub live_pc24: u32,
    pub dead_pc24: u32,
    pub func_entry_pc24: u32,
    pub entry_m: u8,
    pub entry_x: u8,
}

/// Output of `decode_function` for one function entry. `insns` preserves
/// insertion order (the Python used an insertion-ordered dict); lookup is via
/// the `index` side-map.
#[derive(Debug, Clone, Default)]
pub struct FunctionDecodeGraph {
    pub entry: Option<DecodeKey>,
    insns_vec: Vec<DecodedInsn>,
    index: HashMap<DecodeKey, usize>,
    pub suppressed_indirect_calls: Vec<SuppressedIndirectCall>,
    pub const_z_folds: Vec<ConstZFold>,
    pub dispatch_targets_suppressed: Vec<DispatchTargetSuppressed>,
    pub unresolved_indirects: Vec<UnresolvedIndirect>,
    /// PCs explicitly declared as data that are reached by real control flow.
    /// Their bytes are retained for exact execution but BRK/COP-shaped data
    /// does not structurally poison the surrounding function.
    pub data_region_exec_pcs: HashSet<u32>,
    /// Control flow intentionally stopped at a declared function boundary.
    /// The predecessor site and outside target form a tail-exit dependency.
    pub boundary_exits: Vec<(u32, DecodeKey)>,
    /// Direct call sites whose continuation was deliberately truncated
    /// because no architectural callee exit-(M,X) fact was available.
    pub unknown_callee_exit_sites: Vec<(u32, u32, u8, u8)>,
    /// The whole-program solver marks variants whose exit fact changed after
    /// publication.  They remain reachable but are never eligible for AOT.
    pub unstable_exit_fact: bool,
}

impl FunctionDecodeGraph {
    pub fn new(entry: DecodeKey) -> Self {
        FunctionDecodeGraph {
            entry: Some(entry),
            ..Default::default()
        }
    }

    /// Insert or replace a decoded insn, preserving first-insertion order.
    pub fn insert(&mut self, di: DecodedInsn) {
        if let Some(&i) = self.index.get(&di.key) {
            self.insns_vec[i] = di;
        } else {
            let i = self.insns_vec.len();
            self.index.insert(di.key.clone(), i);
            self.insns_vec.push(di);
        }
    }

    pub fn get(&self, key: &DecodeKey) -> Option<&DecodedInsn> {
        self.index.get(key).map(|&i| &self.insns_vec[i])
    }

    pub fn contains(&self, key: &DecodeKey) -> bool {
        self.index.contains_key(key)
    }

    /// All decoded insns in insertion order.
    pub fn insns(&self) -> &[DecodedInsn] {
        &self.insns_vec
    }

    pub fn len(&self) -> usize {
        self.insns_vec.len()
    }

    pub fn is_empty(&self) -> bool {
        self.insns_vec.is_empty()
    }

    /// All DecodeKeys at this 24-bit PC (across entry mode states).
    pub fn keys_at_pc(&self, pc24: u32) -> Vec<DecodeKey> {
        self.insns_vec
            .iter()
            .filter(|d| d.key.pc == pc24)
            .map(|d| d.key.clone())
            .collect()
    }
}

/// Immutable bundle of decode-affecting inputs, threaded by `&` (replaces the
/// Python kwargs + process-global registries). Borrowed maps so the orchestrator
/// can build them once and share across all decodes.
#[derive(Debug, Clone, Default)]
pub struct DecodeEnv<'a> {
    pub rom_mapping: RomMapping,
    /// Per-function decode budget. `None` preserves the public default.
    pub max_insns: Option<usize>,
    pub dispatch_helpers: Option<&'a HashMap<u32, String>>, // target_pc24 -> 'short'|'long'
    pub indirect_call_tables: Option<&'a HashMap<u32, IndirectCallTable>>,
    pub indirect_dispatch: Option<&'a HashMap<u32, IndirectDispatchSite>>,
    pub hle_dispatch: Option<&'a HashMap<u32, String>>,
    pub data_regions: Option<&'a [(u32, u32, u32)]>, // (bank, start, end_excl)
    pub callee_exit_mx: Option<&'a HashMap<(u32, u8, u8), (u8, u8)>>,
    pub callee_exit_mx_modes: Option<&'a HashMap<(u32, u8, u8), Vec<(u8, u8)>>>,
    pub sibling_entry_pcs: Option<&'a std::collections::BTreeSet<u32>>,
    pub reloc_regions: Option<&'a [RelocRegion]>,
    pub callee_inline_skip: Option<&'a HashMap<u32, i32>>,
    pub inline_dispatch_loop_pcs: Option<&'a std::collections::BTreeSet<u32>>,
    pub terminal_jsr_sites: Option<&'a std::collections::BTreeSet<u32>>,
    /// Folded-in process-global the Python decode path read (set_global_inline_skip).
    pub global_inline_skip: Option<&'a HashMap<u32, i32>>,
    /// LLE-first analysis must not guess that a callee preserves M/X.  Stop
    /// the caller graph at an unknown direct call until a later fixed-point
    /// round supplies an exact or multi-mode exit fact.
    pub stop_on_unknown_callee_exit: bool,
}

/// `indirect_call_tables` value shape: {'base': int, 'count': int, 'kind': str}.
#[derive(Debug, Clone)]
pub struct IndirectCallTable {
    pub base: u32,
    pub count: u32,
    pub kind: String,
}

/// `indirect_dispatch` resolved-site value shape: count + idx_reg + table_bases.
#[derive(Debug, Clone)]
pub struct IndirectDispatchSite {
    pub count: u32,
    pub idx_reg: char,         // 'X' | 'Y'
    pub table_bases: Vec<u32>, // 0..3 entries
    pub ptr_call: bool,
    pub pointer_match: bool,
    pub popped_call_frame: bool,
    pub rts_stack: bool,
    pub targets: Vec<u32>,
}

/// Compute (m, x, p_stack) AFTER executing `insn`, given entry state. REP/SEP
/// clear/set M/X per the operand bitmask; PHP pushes the current (m, x); PLP
/// pops and restores it. Other M/X-affecting ops keep the current state.
pub fn post_state(
    insn: &Insn,
    in_m: u8,
    in_x: u8,
    in_p_stack: &[(u8, u8)],
) -> (u8, u8, Vec<(u8, u8)>) {
    let mnem = insn.mnem;
    match mnem {
        "REP" => {
            let m = if insn.operand & 0x20 != 0 { 0 } else { in_m };
            let x = if insn.operand & 0x10 != 0 { 0 } else { in_x };
            (m, x, in_p_stack.to_vec())
        }
        "SEP" => {
            let m = if insn.operand & 0x20 != 0 { 1 } else { in_m };
            let x = if insn.operand & 0x10 != 0 { 1 } else { in_x };
            (m, x, in_p_stack.to_vec())
        }
        "PHP" => {
            if in_p_stack.len() < PHP_STACK_MAX_DEPTH {
                let mut s = in_p_stack.to_vec();
                s.push((in_m, in_x));
                (in_m, in_x, s)
            } else {
                (in_m, in_x, in_p_stack.to_vec())
            }
        }
        "PLP" => {
            if let Some(&(pm, px)) = in_p_stack.last() {
                (pm, px, in_p_stack[..in_p_stack.len() - 1].to_vec())
            } else {
                (in_m, in_x, in_p_stack.to_vec())
            }
        }
        _ => (in_m, in_x, in_p_stack.to_vec()),
    }
}

/// Back-compat: (m, x) without p_stack tracking.
pub fn post_mx(insn: &Insn, in_m: u8, in_x: u8) -> (u8, u8) {
    let (m, x, _) = post_state(insn, in_m, in_x, &[]);
    (m, x)
}

// ── Reloc-aware byte fetch ────────────────────────────────────────────────

/// `addr_to_rom_offset` with the Python AssertionError mapped to `None`: a
/// non-reloc address outside $8000-$FFFF has no LoROM offset.
fn try_rom_offset(
    mapping: RomMapping,
    bank: u32,
    pc16: u32,
    reloc: &[RelocRegion],
) -> Option<usize> {
    if addr_in_reloc_region(bank, pc16, reloc).is_some() {
        return Some(addr_to_rom_offset(mapping, bank, pc16, reloc));
    }
    let a = pc16 & 0xFFFF;
    if is_rom_address(mapping, bank, a) {
        Some(addr_to_rom_offset(mapping, bank, a, reloc))
    } else {
        None
    }
}

// ── Padding / data-region gating ──────────────────────────────────────────

/// True iff the dispatch-table target's bytes look like unmapped ROM padding
/// (all $FF) or cleared region (all $00). Port of `_dispatch_target_is_padding`.
fn dispatch_target_is_padding(
    rom: &[u8],
    mapping: RomMapping,
    bank: u32,
    pc16: u32,
    reloc: &[RelocRegion],
) -> bool {
    const WINDOW: usize = 16;
    let off = match try_rom_offset(mapping, bank, pc16, reloc) {
        Some(o) => o,
        None => return true,
    };
    if off + WINDOW > rom.len() {
        return true;
    }
    let blob = &rom[off..off + WINDOW];
    if blob.iter().all(|&b| b == 0xFF) {
        return true;
    }
    if blob.iter().all(|&b| b == 0x00) {
        return true;
    }
    false
}

/// True iff (bank, pc16) is inside any cfg `data_region`. Port of
/// `_addr_in_data_regions`.
fn addr_in_data_regions(data_regions: Option<&[(u32, u32, u32)]>, bank: u32, pc16: u32) -> bool {
    let dr = match data_regions {
        Some(d) if !d.is_empty() => d,
        _ => return false,
    };
    let pc16 = pc16 & 0xFFFF;
    let bank = bank & 0xFF;
    for &(b, s, e) in dr {
        if (b & 0xFF) != bank {
            continue;
        }
        if (s & 0xFFFF) <= pc16 && pc16 < (e & 0xFFFF) {
            return true;
        }
    }
    false
}

// ── Auto-recovery for indirect dispatch ───────────────────────────────────

/// Resolved indirect-dispatch authorisation (cfg directive or auto-recovered).
struct Auth {
    count: u32,
    idx_reg: char,
    table_bases: Vec<u32>,
    ptr_call: bool,
    pointer_match: bool,
    popped_call_frame: bool,
    targets: Vec<u32>,
    local_goto: bool,
}

fn is_long_dispatch(insn: &Insn, table_bases: &[u32]) -> bool {
    // Opcode $DC is the three-byte `JML [abs]` form: instruction length is
    // not the target width. Parallel tables with an explicit bank plane and
    // ordinary four-byte JML/JSL encodings are long for the same reason.
    insn.opcode == 0xDC || insn.length == 4 || table_bases.len() == 3
}

/// Walk the dispatch table for a `JMP/JML/JSR (abs,X)`. Port of
/// `_autorecover_indirect_xtable`.
fn autorecover_indirect_xtable(
    rom: &[u8],
    mapping: RomMapping,
    bank: u32,
    insn: &Insn,
    data_regions: Option<&[(u32, u32, u32)]>,
    reloc: &[RelocRegion],
    func_start: u32,
) -> Option<Vec<u32>> {
    let base = insn.operand & 0xFFFF;
    let entry_size: u32 = if is_long_dispatch(insn, &[]) { 3 } else { 2 };
    let max_entries = 256usize;
    let mut entries: Vec<u32> = Vec::new();
    let mut tbl_pc = base;
    let mut nulls_in_a_row = 0;
    let code_boundary: Option<u32> = if base < (func_start & 0xFFFF) {
        Some(func_start & 0xFFFF)
    } else {
        None
    };
    let mut inbank_handler_pcs: Vec<u32> = Vec::new();
    while entries.len() < max_entries {
        if tbl_pc + entry_size - 1 > 0xFFFF {
            break;
        }
        if let Some(cb) = code_boundary {
            if tbl_pc >= cb {
                break;
            }
        }
        if inbank_handler_pcs.iter().any(|&h| tbl_pc >= h) {
            break;
        }
        let off = match try_rom_offset(mapping, bank, tbl_pc & 0xFFFF, reloc) {
            Some(o) => o,
            None => break,
        };
        if off + (entry_size as usize) > rom.len() {
            break;
        }
        let addr16 = rom[off] as u32 | ((rom[off + 1] as u32) << 8);
        let (eb, full) = if entry_size == 3 {
            let eb = rom[off + 2] as u32;
            (eb, (eb << 16) | addr16)
        } else {
            (bank, (bank << 16) | addr16)
        };
        if addr16 == 0 && (entry_size == 2 || eb == 0) {
            nulls_in_a_row += 1;
            if nulls_in_a_row >= 2 {
                if entries.last() == Some(&0) {
                    entries.pop();
                }
                break;
            }
            entries.push(0);
            tbl_pc += entry_size;
            continue;
        }
        nulls_in_a_row = 0;
        if addr16 < 0x8000 {
            break;
        }
        if addr_in_data_regions(data_regions, eb, addr16) {
            break;
        }
        if dispatch_target_is_padding(rom, mapping, eb, addr16, reloc) {
            break;
        }
        entries.push(full);
        if eb == bank && base <= addr16 && addr16 <= 0xFFFF {
            inbank_handler_pcs.push(addr16);
        }
        tbl_pc += entry_size;
    }
    if entries.is_empty() {
        None
    } else {
        Some(entries)
    }
}

/// Recover the same-function stride runway used by Duff-style computed JMPs.
/// Port of Python's `_autorecover_local_stride_runway`.
fn autorecover_local_stride_runway(
    rom: &[u8],
    mapping: RomMapping,
    bank: u32,
    func_start: u32,
    site_pc: u32,
    insn: &Insn,
    end: Option<u32>,
    data_regions: Option<&[(u32, u32, u32)]>,
    reloc: &[RelocRegion],
) -> Option<Vec<u32>> {
    if insn.mnem != "JMP" || insn.mode != Mode::Indir || insn.length != 3 {
        return None;
    }
    let dp = insn.operand & 0xFFFF;
    if dp > 0xFF {
        return None;
    }
    let read8 = |pc: u32| -> Option<u8> {
        if !(0x8000..=0xFFFF).contains(&pc) {
            return None;
        }
        let offset = try_rom_offset(mapping, bank, pc, reloc)?;
        rom.get(offset).copied()
    };

    let site_pc = site_pc & 0xFFFF;
    let pattern_start = site_pc.checked_sub(17)?;
    if pattern_start < 0x8000 {
        return None;
    }
    let expected = [
        (0, 0x4A),
        (1, 0x85),
        (2, dp as u8),
        (3, 0x4A),
        (4, 0x65),
        (5, dp as u8),
        (6, 0x18),
        (7, 0x69),
        (10, 0x85),
        (11, dp as u8),
        (12, 0xA9),
        (15, 0xE2),
        (16, 0x30),
        (17, 0x6C),
        (18, dp as u8),
        (19, 0x00),
    ];
    if expected
        .iter()
        .any(|&(relative, byte)| read8(pattern_start + relative) != Some(byte))
    {
        return None;
    }

    let base = read8(pattern_start + 8)? as u32 | ((read8(pattern_start + 9)? as u32) << 8);
    let start = func_start & 0xFFFF;
    if !(0x8000..=0xFFFF).contains(&base)
        || base < start
        || end.is_some_and(|limit| base >= (limit & 0xFFFF))
        || addr_in_data_regions(data_regions, bank, base)
        || dispatch_target_is_padding(rom, mapping, bank, base, reloc)
    {
        return None;
    }

    let mut entries = Vec::new();
    let mut first_store_addr = None;
    for index in 0..4096u32 {
        let target = base + index * 3;
        if target + 2 > 0xFFFF
            || end.is_some_and(|limit| target >= (limit & 0xFFFF))
            || addr_in_data_regions(data_regions, bank, target)
        {
            break;
        }
        let Some(opcode) = read8(target) else {
            break;
        };
        let Some(lo) = read8(target + 1) else {
            break;
        };
        let Some(hi) = read8(target + 2) else {
            break;
        };
        if opcode != 0x8D {
            break;
        }
        let store_addr = lo as u32 | ((hi as u32) << 8);
        match first_store_addr {
            None => first_store_addr = Some(store_addr),
            Some(first) if store_addr != ((first + index * 4) & 0xFFFF) => break,
            _ => {}
        }
        entries.push((bank << 16) | target);
    }
    (entries.len() >= 4).then_some(entries)
}

/// Walk back from func start to find the LDA/STA pairs that compose a DP
/// dispatch pointer. Port of `_autorecover_indirect_dp`.
fn autorecover_indirect_dp(
    rom: &[u8],
    mapping: RomMapping,
    bank: u32,
    func_start: u32,
    site_pc: u32,
    dp_addr: u32,
    insn_length: u8,
    reloc: &[RelocRegion],
) -> Option<(Vec<u32>, char)> {
    let mut winners: HashMap<i64, (u32, char)> = HashMap::new();
    let mut pc = func_start & 0xFFFF;
    let mut m_state = 1u8;
    let mut x_state = 1u8;
    let mut last_lda_table: Option<(u32, char)> = None;
    let mut scanned = 0;
    let max_scan = 256;
    while pc < site_pc && scanned < max_scan {
        let off = try_rom_offset(mapping, bank, pc, reloc)?;
        if off >= rom.len() {
            return None;
        }
        let insn = decode_insn(rom, off, pc, bank, m_state, x_state)?;
        let mnem = insn.mnem;
        if mnem == "REP" {
            if insn.operand & 0x20 != 0 {
                m_state = 0;
            }
            if insn.operand & 0x10 != 0 {
                x_state = 0;
            }
        } else if mnem == "SEP" {
            if insn.operand & 0x20 != 0 {
                m_state = 1;
            }
            if insn.operand & 0x10 != 0 {
                x_state = 1;
            }
        }
        if mnem == "LDA" && (insn.mode == Mode::AbsX || insn.mode == Mode::LongX) {
            last_lda_table = Some((insn.operand & 0xFFFF, 'X'));
        } else if mnem == "LDA" && insn.mode == Mode::AbsY {
            last_lda_table = Some((insn.operand & 0xFFFF, 'Y'));
        } else if mnem == "STA" && insn.mode == Mode::Dp {
            let slot = (insn.operand & 0xFFFF) as i64 - (dp_addr & 0xFFFF) as i64;
            if (0..=2).contains(&slot) {
                if let Some(lda) = last_lda_table.take() {
                    winners.insert(slot, lda);
                }
            }
        } else if mnem == "STA" || mnem == "STZ" {
            let slot = (insn.operand & 0xFFFF) as i64 - (dp_addr & 0xFFFF) as i64;
            if (0..=2).contains(&slot) {
                winners.remove(&slot);
            }
        } else if mnem == "LDA" {
            last_lda_table = None;
        }
        scanned += 1;
        pc = (pc + insn.length as u32) & 0xFFFF;
    }

    if winners.is_empty() {
        return None;
    }
    let idx_regs: HashSet<char> = winners.values().map(|w| w.1).collect();
    if idx_regs.len() != 1 {
        return None;
    }
    let idx_reg = *idx_regs.iter().next().unwrap();
    let needed_slots = if insn_length == 3 && m_state == 1 && winners.contains_key(&2) {
        3
    } else if winners.contains_key(&1) {
        2
    } else {
        1
    };
    let mut table_bases = Vec::new();
    for s in 0..needed_slots {
        let winner = winners.get(&(s as i64))?;
        table_bases.push(winner.0);
    }
    Some((table_bases, idx_reg))
}

/// Count valid entries in a DP-pointer dispatch's parallel byte-tables. Port of
/// `_autorecover_dp_table_count`.
fn autorecover_dp_table_count(
    rom: &[u8],
    mapping: RomMapping,
    bank: u32,
    table_bases: &[u32],
    data_regions: Option<&[(u32, u32, u32)]>,
    reloc: &[RelocRegion],
) -> Option<u32> {
    if table_bases.is_empty() {
        return None;
    }
    let max_entries = 256u32;
    if table_bases.len() == 1 {
        let base = table_bases[0] & 0xFFFF;
        let mut count = 0u32;
        for i in 0..max_entries {
            let tbl_pc = (base + 2 * i) & 0xFFFF;
            if tbl_pc + 1 > 0xFFFF {
                break;
            }
            let off = match try_rom_offset(mapping, bank, tbl_pc, reloc) {
                Some(o) => o,
                None => break,
            };
            if off + 1 >= rom.len() {
                break;
            }
            let addr16 = rom[off] as u32 | ((rom[off + 1] as u32) << 8);
            if addr16 == 0 {
                break;
            }
            if addr16 < 0x8000 {
                break;
            }
            if addr_in_data_regions(data_regions, bank, addr16) {
                break;
            }
            if dispatch_target_is_padding(rom, mapping, bank, addr16, reloc) {
                break;
            }
            count += 1;
        }
        return if count > 0 { Some(count) } else { None };
    }
    let lo_base = table_bases[0] & 0xFFFF;
    let hi_base = table_bases[1] & 0xFFFF;
    let bk_base = if table_bases.len() >= 3 {
        Some(table_bases[2] & 0xFFFF)
    } else {
        None
    };
    let mut count = 0u32;
    for i in 0..max_entries {
        let lo_off = match try_rom_offset(mapping, bank, (lo_base + i) & 0xFFFF, reloc) {
            Some(o) => o,
            None => break,
        };
        let hi_off = match try_rom_offset(mapping, bank, (hi_base + i) & 0xFFFF, reloc) {
            Some(o) => o,
            None => break,
        };
        if lo_off.max(hi_off) >= rom.len() {
            break;
        }
        let lo = rom[lo_off] as u32;
        let hi = rom[hi_off] as u32;
        let eb = if let Some(bk) = bk_base {
            let bk_off = match try_rom_offset(mapping, bank, (bk + i) & 0xFFFF, reloc) {
                Some(o) => o,
                None => break,
            };
            if bk_off >= rom.len() {
                break;
            }
            rom[bk_off] as u32
        } else {
            bank
        };
        let addr16 = (hi << 8) | lo;
        if addr16 == 0 {
            break;
        }
        if addr16 < 0x8000 {
            break;
        }
        if addr_in_data_regions(data_regions, eb, addr16) {
            break;
        }
        if dispatch_target_is_padding(rom, mapping, eb, addr16, reloc) {
            break;
        }
        count += 1;
    }
    if count > 0 {
        Some(count)
    } else {
        None
    }
}

/// Read N dispatch targets from ROM per an `Auth`. Port of
/// `_resolve_indirect_dispatch_targets`.
fn resolve_indirect_dispatch_targets(
    rom: &[u8],
    mapping: RomMapping,
    bank: u32,
    insn: &Insn,
    count: u32,
    bases: &[u32],
    targets: &[u32],
    reloc: &[RelocRegion],
) -> Option<Vec<u32>> {
    if !targets.is_empty() {
        if targets.len() != count as usize {
            return None;
        }
        return targets
            .iter()
            .copied()
            .map(|target| {
                if target > 0xFFFFFF {
                    None
                } else if target == 0 {
                    // Preserve an explicit null slot instead of turning it
                    // into bank:$0000; every downstream consumer skips zero.
                    Some(0)
                } else if target > 0xFFFF {
                    Some(target)
                } else {
                    Some((bank << 16) | target)
                }
            })
            .collect();
    }
    if bases.len() >= 2 {
        let lo_base = bases[0] & 0xFFFF;
        let hi_base = bases[1] & 0xFFFF;
        let bk_base = if bases.len() == 3 {
            Some(bases[2] & 0xFFFF)
        } else {
            None
        };
        let mut entries = Vec::new();
        for i in 0..count {
            let lo_off = try_rom_offset(mapping, bank, (lo_base + i) & 0xFFFF, reloc)?;
            let hi_off = try_rom_offset(mapping, bank, (hi_base + i) & 0xFFFF, reloc)?;
            if lo_off.max(hi_off) >= rom.len() {
                return None;
            }
            let lo = rom[lo_off] as u32;
            let hi = rom[hi_off] as u32;
            if let Some(bk) = bk_base {
                let bk_off = try_rom_offset(mapping, bank, (bk + i) & 0xFFFF, reloc)?;
                if bk_off >= rom.len() {
                    return None;
                }
                let eb = rom[bk_off] as u32;
                entries.push((eb << 16) | (hi << 8) | lo);
            } else {
                entries.push((bank << 16) | (hi << 8) | lo);
            }
        }
        return Some(entries);
    }

    let base = if !bases.is_empty() {
        bases[0] & 0xFFFF
    } else {
        insn.operand & 0xFFFF
    };
    let entry_size: u32 = if is_long_dispatch(insn, bases) { 3 } else { 2 };
    let mut entries = Vec::new();
    let mut tbl_pc = base;
    for _ in 0..count {
        if tbl_pc + entry_size - 1 > 0xFFFF {
            return None;
        }
        let off = try_rom_offset(mapping, bank, tbl_pc & 0xFFFF, reloc)?;
        if off + (entry_size as usize) > rom.len() {
            return None;
        }
        let addr16 = rom[off] as u32 | ((rom[off + 1] as u32) << 8);
        if entry_size == 3 {
            let eb = rom[off + 2] as u32;
            entries.push((eb << 16) | addr16);
        } else {
            entries.push((bank << 16) | addr16);
        }
        tbl_pc += entry_size;
    }
    Some(entries)
}

// ── Successor labelling ───────────────────────────────────────────────────

/// Return the hardware RTS destination for `PEA <ret-1>; JMP (ptr)`.
fn pea_ptrcall_return_pc(
    rom: &[u8],
    mapping: RomMapping,
    bank: u32,
    pc: u32,
    fallback_next: u32,
    reloc: &[RelocRegion],
) -> u32 {
    let prev_pc = pc.wrapping_sub(3) & 0xFFFF;
    let Some(off) = try_rom_offset(mapping, bank, prev_pc, reloc) else {
        return fallback_next & 0xFFFF;
    };
    if off + 2 >= rom.len() || rom[off] != 0xF4 {
        return fallback_next & 0xFFFF;
    }
    let pea_operand = rom[off + 1] as u32 | ((rom[off + 2] as u32) << 8);
    (pea_operand + 1) & 0xFFFF
}

/// Compute (DecodeKey, edge_kind) successor tuples for one decoded insn. Port of
/// `_labeled_successors`.
fn labeled_successors(
    insn: &Insn,
    key: &DecodeKey,
    bank: u32,
    env: &DecodeEnv,
    _rom: &[u8],
    unknown_callee_exit_sites: &mut Vec<(u32, u32, u8, u8)>,
) -> Vec<(DecodeKey, &'static str)> {
    let (post_m, post_x, post_p_stack) = post_state(insn, key.m, key.x, &key.p_stack);
    let pc = insn.addr & 0xFFFF;
    let next_pc = (pc + insn.length as u32) & 0xFFFF;
    let mnem = insn.mnem;

    if is_terminator(mnem) {
        return vec![];
    }
    if mnem == "BRA" || mnem == "BRL" {
        return vec![(
            DecodeKey::with_stack(addr24(bank, insn.operand), post_m, post_x, post_p_stack),
            "jump",
        )];
    }
    if is_cond_branch(mnem) {
        return vec![
            (
                DecodeKey::with_stack(addr24(bank, next_pc), post_m, post_x, post_p_stack.clone()),
                "fall",
            ),
            (
                DecodeKey::with_stack(addr24(bank, insn.operand), post_m, post_x, post_p_stack),
                "jump",
            ),
        ];
    }
    if mnem == "JMP" {
        if insn.mode == Mode::Abs {
            return vec![(
                DecodeKey::with_stack(addr24(bank, insn.operand), post_m, post_x, post_p_stack),
                "jump",
            )];
        }
        if insn.mode == Mode::Long {
            // JML is a cross-routine tail transfer even when its explicit
            // bank byte happens to equal the current bank. The program
            // analyzer records a direct-tail demand; importing the target
            // body here would merge two ABI/exit domains.
            return vec![];
        }
        return vec![];
    }

    if mnem == "JSR" || mnem == "JSL" {
        if insn.terminal_jsr {
            return vec![];
        }
        let (mut ret_m, mut ret_x) = (post_m, post_x);
        let target_pc24: Option<u32> =
            if mnem == "JSR" && insn.length == 3 && insn.mode != Mode::IndirX {
                Some(addr24(bank, insn.operand & 0xFFFF))
            } else if mnem == "JSL" {
                Some(insn.operand & 0xFFFFFF)
            } else {
                None
            };
        let mut eff_next_pc = next_pc;
        let skip_map = env.callee_inline_skip.or(env.global_inline_skip);
        if let (Some(map), Some(tp)) = (skip_map, target_pc24) {
            let mut skip = map.get(&tp).copied();
            if skip.is_none() {
                let tbank = (tp >> 16) & 0xFF;
                if tbank < 0x40 || (0x80..0xC0).contains(&tbank) {
                    skip = map.get(&(tp ^ 0x800000)).copied();
                }
            }
            if let Some(s) = skip {
                if s != 0 {
                    eff_next_pc = (((next_pc as i64) + s as i64) & 0xFFFF) as u32;
                }
            }
        }
        let mut callee_exit_known = false;
        if let Some(cem) = env.callee_exit_mx {
            if let Some(tp) = target_pc24 {
                let mut hit = cem.get(&(tp, post_m, post_x)).copied();
                if hit.is_none() {
                    let tbank = (tp >> 16) & 0xFF;
                    if tbank < 0x40 || (0x80..0xC0).contains(&tbank) {
                        hit = cem.get(&(tp ^ 0x800000, post_m, post_x)).copied();
                    }
                }
                if let Some((em, ex)) = hit {
                    ret_m = em & 1;
                    ret_x = ex & 1;
                    callee_exit_known = true;
                }
            }
        }
        if let (Some(tp), Some(cmm)) = (target_pc24, env.callee_exit_mx_modes) {
            if !callee_exit_known {
                let mut mode_set: Option<Vec<(u8, u8)>> = cmm.get(&(tp, post_m, post_x)).cloned();
                if mode_set.is_none() {
                    let tbank = (tp >> 16) & 0xFF;
                    if tbank < 0x40 || (0x80..0xC0).contains(&tbank) {
                        mode_set = cmm.get(&(tp ^ 0x800000, post_m, post_x)).cloned();
                    }
                }
                if let Some(v) = mode_set {
                    let mut pairs: Vec<(u8, u8)> = v.iter().map(|&(m, x)| (m & 1, x & 1)).collect();
                    pairs.sort();
                    pairs.dedup();
                    if pairs.is_empty() {
                        // A proven empty exit set means the callee never
                        // resumes this call site. This is a terminal edge,
                        // not an unknown return width.
                        return vec![];
                    }
                    let mut succs = Vec::new();
                    for (em, ex) in pairs {
                        succs.push((
                            DecodeKey::with_stack(
                                addr24(bank, eff_next_pc),
                                em,
                                ex,
                                post_p_stack.clone(),
                            ),
                            "fall",
                        ));
                    }
                    if !succs.is_empty() {
                        return succs;
                    }
                }
            }
        }
        if env.stop_on_unknown_callee_exit && !callee_exit_known {
            if let Some(target_pc24) = target_pc24 {
                let item = (insn.addr & 0xFFFFFF, target_pc24, post_m & 1, post_x & 1);
                if !unknown_callee_exit_sites.contains(&item) {
                    unknown_callee_exit_sites.push(item);
                }
                return vec![];
            }
        }
        return vec![(
            DecodeKey::with_stack(addr24(bank, eff_next_pc), ret_m, ret_x, post_p_stack),
            "fall",
        )];
    }

    vec![(
        DecodeKey::with_stack(addr24(bank, next_pc), post_m, post_x, post_p_stack),
        "fall",
    )]
}

// ── Main worklist walk ────────────────────────────────────────────────────

/// Decode a function starting at (bank, start) with entry (m, x) state. Port of
/// `_decode_function_uncached`.
pub fn decode_function(
    rom: &[u8],
    bank: u32,
    start: u32,
    entry_m: u8,
    entry_x: u8,
    end: Option<u32>,
    env: &DecodeEnv,
) -> FunctionDecodeGraph {
    let mapping = env.rom_mapping;
    let reloc: &[RelocRegion] = env.reloc_regions.unwrap_or(&[]);
    let data_regions = env.data_regions;
    let max_insns = env.max_insns.unwrap_or(4096);

    let entry_m = entry_m & 1;
    let entry_x = entry_x & 1;
    let entry_key = DecodeKey::new(addr24(bank, start), entry_m, entry_x);
    let mut graph = FunctionDecodeGraph::new(entry_key.clone());

    let mut inline_loop_sites: HashSet<u32> = HashSet::new();
    if let Some(s) = env.inline_dispatch_loop_pcs {
        for &p in s {
            inline_loop_sites.insert(p & 0xFFFF);
        }
    }
    let mut inline_pcs: HashSet<u32> = HashSet::new();

    // (key, edge_kind, pred_pc). pred_pc = -1 for the entry seed.
    let mut worklist: Vec<(DecodeKey, &'static str, i64)> = vec![(entry_key, "entry", -1)];

    while let Some((key, edge_kind, pred_pc)) = worklist.pop() {
        if graph.len() >= max_insns {
            panic!(
                "v2 decoder exceeded max_insns={max_insns} at ${:06X}",
                addr24(bank, start)
            );
        }
        if graph.contains(&key) {
            continue;
        }
        let pc = key.pc & 0xFFFF;

        let pred_in_territory = pred_pc >= 0 && inline_pcs.contains(&((pred_pc as u32) & 0xFFFF));
        if pred_in_territory {
            inline_pcs.insert(pc);
        }

        // Boundary-crossing fall-through past end:.
        if let Some(end_v) = end {
            if pc >= end_v && edge_kind == "fall" && pred_pc >= 0 && (pred_pc as u32) < end_v {
                let boundary = (addr24(bank, pred_pc as u32), key.clone());
                if !graph.boundary_exits.contains(&boundary) {
                    graph.boundary_exits.push(boundary);
                }
                continue;
            }
        }
        // Jump edge onto a named sibling function entry.
        if let Some(sib) = env.sibling_entry_pcs {
            if edge_kind == "jump" && sib.contains(&pc) && !pred_in_territory {
                let boundary = (addr24(bank, pred_pc as u32), key.clone());
                if !graph.boundary_exits.contains(&boundary) {
                    graph.boundary_exits.push(boundary);
                }
                continue;
            }
        }
        // Address-range gate.
        let in_reloc = addr_in_reloc_region(bank, pc, reloc).is_some();
        if !in_reloc && !is_rom_address(mapping, bank, pc) {
            continue;
        }
        let offset = match try_rom_offset(mapping, bank, pc, reloc) {
            Some(o) => o,
            None => continue,
        };
        if offset >= rom.len() {
            continue;
        }
        if addr_in_data_regions(data_regions, bank, pc) {
            graph.data_region_exec_pcs.insert(addr24(bank, pc));
        }

        let mut insn = decode_insn(rom, offset, pc, bank, key.m, key.x)
            .unwrap_or_else(|| panic!("v2 decoder: unknown opcode at ${bank:02X}:{pc:04X}"));
        insn.m_flag = key.m;
        insn.x_flag = key.x;
        if env
            .terminal_jsr_sites
            .is_some_and(|sites| sites.contains(&(insn.addr & 0xFFFFFF)))
        {
            assert!(
                insn.mnem == "JSR" && insn.mode != Mode::IndirX && insn.length == 3,
                "terminal_jsr at ${:06X} does not name a direct three-byte JSR",
                insn.addr & 0xFFFFFF
            );
            insn.terminal_jsr = true;
        }

        // JSL/JML dispatch-helper table.
        let is_jsl_or_jml = insn.mnem == "JSL" || (insn.mnem == "JMP" && insn.length == 4);
        let helper_kind: Option<String> = if is_jsl_or_jml {
            env.dispatch_helpers
                .and_then(|m| m.get(&(insn.operand & 0xFFFFFF)).cloned())
        } else {
            None
        };
        if let Some(hk) = helper_kind {
            let entry_size: u32 = if hk == "long" { 3 } else { 2 };
            let mut entries: Vec<u32> = Vec::new();
            let mut tbl_pc = (pc + insn.length as u32) & 0xFFFF;
            while entries.len() < 256 && tbl_pc + entry_size - 1 <= 0xFFFF {
                // Bound the inline dispatch table at the next declared function
                // entry. A sibling entry marks the start of the following
                // routine, so its bytes cannot also be table data; without this
                // bound the scan runs into that routine and misreads its opening
                // instructions as extra table entries (e.g. SMW
                // ProcessPlayerAnimation JSL ExecutePtr $00C595: 14 real entries
                // $C599..$C5B4, otherwise 2 more are read from
                // PlayerState0B_RescuedPeach $C5B5, corrupting the dispatch and
                // fabricating targets $DE9C/$9C13). This can only tighten the
                // scan; a spuriously short table simply routes uncovered indices
                // to the interpreter.
                if !entries.is_empty() {
                    if let Some(sib) = env.sibling_entry_pcs {
                        if sib.contains(&(tbl_pc & 0xFFFF)) {
                            break;
                        }
                    }
                }
                let tbl_off = match try_rom_offset(mapping, bank, tbl_pc, reloc) {
                    Some(o) => o,
                    None => break,
                };
                if tbl_off + (entry_size as usize) > rom.len() {
                    break;
                }
                let lo = rom[tbl_off] as u32;
                let hi = rom[tbl_off + 1] as u32;
                let addr16 = lo | (hi << 8);
                if hk == "long" {
                    let eb = rom[tbl_off + 2] as u32;
                    if addr16 == 0 && eb == 0 {
                        entries.push(0);
                        tbl_pc += entry_size;
                        continue;
                    }
                    if addr16 < 0x8000 && addr_in_reloc_region(eb, addr16, reloc).is_none() {
                        break;
                    }
                    let is_valid_lorom_bank = !(0x40..0x80).contains(&eb);
                    if !is_valid_lorom_bank {
                        break;
                    }
                    if dispatch_target_is_padding(rom, mapping, eb, addr16, reloc) {
                        break;
                    }
                    if addr_in_data_regions(data_regions, eb, addr16) {
                        graph
                            .dispatch_targets_suppressed
                            .push(DispatchTargetSuppressed {
                                site_pc24: (bank << 16) | pc,
                                target_pc24: (eb << 16) | addr16,
                                reason: "data_region".to_string(),
                                table_index: entries.len() as u32,
                            });
                        break;
                    }
                    entries.push((eb << 16) | addr16);
                } else {
                    if addr16 == 0 {
                        entries.push(0);
                        tbl_pc += entry_size;
                        continue;
                    }
                    if addr16 < 0x8000 && addr_in_reloc_region(bank, addr16, reloc).is_none() {
                        break;
                    }
                    if dispatch_target_is_padding(rom, mapping, bank, addr16, reloc) {
                        break;
                    }
                    if addr_in_data_regions(data_regions, bank, addr16) {
                        graph
                            .dispatch_targets_suppressed
                            .push(DispatchTargetSuppressed {
                                site_pc24: (bank << 16) | pc,
                                target_pc24: (bank << 16) | addr16,
                                reason: "data_region".to_string(),
                                table_index: entries.len() as u32,
                            });
                        break;
                    }
                    entries.push(addr16);
                }
                tbl_pc += entry_size;
            }
            if !entries.is_empty() {
                insn.dispatch_entries = Some(entries);
                insn.dispatch_kind = Some(hk);
                graph.insert(DecodedInsn {
                    key,
                    insn,
                    successors: vec![],
                });
                continue;
            }
        }

        // cfg/auto indirect_dispatch for JMP/JML indirect.
        if insn.mnem == "JMP" && (insn.mode == Mode::Indir || insn.mode == Mode::IndirX) {
            let site_pc24 = (bank << 16) | pc;
            let mut auth: Option<Auth> =
                env.indirect_dispatch
                    .and_then(|m| m.get(&site_pc24))
                    .map(|s| Auth {
                        count: s.count,
                        idx_reg: s.idx_reg,
                        table_bases: s.table_bases.clone(),
                        ptr_call: s.ptr_call,
                        pointer_match: s.pointer_match,
                        popped_call_frame: s.popped_call_frame,
                        targets: s.targets.clone(),
                        local_goto: false,
                    });
            if auth.is_none() && insn.mode == Mode::IndirX {
                if let Some(entries) = autorecover_indirect_xtable(
                    rom,
                    mapping,
                    bank,
                    &insn,
                    data_regions,
                    reloc,
                    start,
                ) {
                    auth = Some(Auth {
                        count: entries.len() as u32,
                        idx_reg: 'X',
                        table_bases: vec![],
                        ptr_call: false,
                        pointer_match: false,
                        popped_call_frame: false,
                        // Preserve tolerated null slots; re-reading only the
                        // count would turn raw $0000 into bank:$0000.
                        targets: entries,
                        local_goto: false,
                    });
                }
            }
            if auth.is_none() && insn.mode == Mode::Indir {
                let dp_op = insn.operand & 0xFFFF;
                if dp_op <= 0x00FF {
                    if let Some((table_bases, idx_reg)) = autorecover_indirect_dp(
                        rom,
                        mapping,
                        bank,
                        start,
                        pc,
                        dp_op,
                        insn.length,
                        reloc,
                    ) {
                        if let Some(count) = autorecover_dp_table_count(
                            rom,
                            mapping,
                            bank,
                            &table_bases,
                            data_regions,
                            reloc,
                        ) {
                            auth = Some(Auth {
                                count,
                                idx_reg,
                                table_bases,
                                ptr_call: false,
                                pointer_match: false,
                                popped_call_frame: false,
                                targets: vec![],
                                local_goto: false,
                            });
                        }
                    }
                }
            }
            if auth.is_none() && insn.mode == Mode::Indir && (insn.operand & 0xFFFF) >= 0x8000 {
                let tbl_pc = insn.operand & 0xFFFF;
                let need = if is_long_dispatch(&insn, &[]) {
                    3usize
                } else {
                    2usize
                };
                let rom_ok = match try_rom_offset(mapping, bank, tbl_pc, reloc) {
                    Some(tbl_off) => tbl_off + need - 1 < rom.len(),
                    None => false,
                };
                if rom_ok {
                    let tbl_off = try_rom_offset(mapping, bank, tbl_pc, reloc).unwrap();
                    let tgt16 = rom[tbl_off] as u32 | ((rom[tbl_off + 1] as u32) << 8);
                    if tgt16 >= 0x8000
                        && !addr_in_data_regions(data_regions, bank, tgt16)
                        && !dispatch_target_is_padding(rom, mapping, bank, tgt16, reloc)
                    {
                        auth = Some(Auth {
                            count: 1,
                            idx_reg: 'X',
                            table_bases: vec![tbl_pc],
                            ptr_call: false,
                            pointer_match: false,
                            popped_call_frame: false,
                            targets: vec![],
                            local_goto: false,
                        });
                    }
                }
            }
            if auth.is_none() && insn.mode == Mode::Indir {
                if let Some(entries) = autorecover_local_stride_runway(
                    rom,
                    mapping,
                    bank,
                    start,
                    pc,
                    &insn,
                    end,
                    data_regions,
                    reloc,
                ) {
                    auth = Some(Auth {
                        count: entries.len() as u32,
                        idx_reg: 'X',
                        table_bases: vec![insn.operand & 0xFFFF],
                        ptr_call: false,
                        pointer_match: false,
                        popped_call_frame: false,
                        targets: entries,
                        local_goto: true,
                    });
                }
            }
            if let Some(auth) = auth {
                if let Some(entries) = resolve_indirect_dispatch_targets(
                    rom,
                    mapping,
                    bank,
                    &insn,
                    auth.count,
                    &auth.table_bases,
                    &auth.targets,
                    reloc,
                ) {
                    insn.dispatch_entries = Some(entries.clone());
                    insn.dispatch_kind = Some(
                        if is_long_dispatch(&insn, &auth.table_bases) {
                            "long"
                        } else {
                            "short"
                        }
                        .to_string(),
                    );
                    insn.dispatch_idx_reg = Some(auth.idx_reg);
                    insn.dispatch_table_bases = if auth.pointer_match || auth.ptr_call {
                        vec![insn.operand & 0xFFFF]
                    } else {
                        auth.table_bases.clone()
                    };
                    insn.dispatch_local_goto = auth.local_goto;
                    insn.dispatch_call = auth.ptr_call;
                    insn.dispatch_pointer_match = auth.pointer_match;
                    insn.dispatch_popped_call_frame = auth.popped_call_frame;
                    if auth.ptr_call {
                        insn.dispatch_consumed_stack_bytes =
                            if is_long_dispatch(&insn, &auth.table_bases) {
                                3
                            } else {
                                2
                            };
                    }
                    if inline_loop_sites.contains(&pc) {
                        insn.inline_dispatch_loop = true;
                        inline_pcs.insert(pc);
                    }
                    let site_m = insn.m_flag & 1;
                    let site_x = insn.x_flag & 1;
                    let mut extra_succs: Vec<(DecodeKey, &'static str)> = Vec::new();
                    for &e in &entries {
                        if e == 0 {
                            continue;
                        }
                        let eb = (e >> 16) & 0xFF;
                        let e16 = e & 0xFFFF;
                        if eb == bank
                            && ((0x8000..=0xFFFF).contains(&e16)
                                || addr_in_reloc_region(eb, e16, reloc).is_some())
                        {
                            extra_succs
                                .push((DecodeKey::new(addr24(eb, e16), site_m, site_x), "jump"));
                        }
                    }
                    let succ: Vec<DecodeKey> = if auth.ptr_call {
                        let next_pc = pea_ptrcall_return_pc(
                            rom,
                            mapping,
                            bank,
                            pc,
                            (pc + insn.length as u32) & 0xFFFF,
                            reloc,
                        );
                        let next = DecodeKey::new(addr24(bank, next_pc), site_m, site_x);
                        extra_succs.push((next.clone(), "fall"));
                        vec![next]
                    } else {
                        extra_succs.iter().map(|(k, _)| k.clone()).collect()
                    };
                    graph.insert(DecodedInsn {
                        key,
                        insn,
                        successors: succ,
                    });
                    for (s, sk) in extra_succs {
                        if !graph.contains(&s) {
                            worklist.push((s, sk, pc as i64));
                        }
                    }
                    continue;
                }
            }
        }

        // Unresolved indirect JMP/JML (or hle_dispatch-claimed).
        if insn.mnem == "JMP" && (insn.mode == Mode::Indir || insn.mode == Mode::IndirX) {
            let mnem_s = insn.mnem.to_string();
            let mode = insn.mode;
            let operand = insn.operand & 0xFFFFFF;
            let (km, kx) = (key.m, key.x);
            graph.insert(DecodedInsn {
                key,
                insn,
                successors: vec![],
            });
            if let Some(hd) = env.hle_dispatch {
                if hd.contains_key(&pc) {
                    continue;
                }
            }
            graph.unresolved_indirects.push(UnresolvedIndirect {
                site_pc24: (bank << 16) | pc,
                mnem: mnem_s,
                mode,
                operand,
                function_entry_pc24: addr24(bank, start),
                entry_m: km,
                entry_x: kx,
            });
            continue;
        }

        // Explicit PEI;RTS internal computed transfer. DKC2 stores a
        // return-address-minus-one in DP, PEI pushes it, and the following
        // RTS jumps to one of the finite cfg targets. This is a same-function
        // goto: it creates no whole-program call demand and preserves M/X.
        if insn.mnem == "PEI" {
            let site_pc24 = (bank << 16) | pc;
            if let Some(site) = env
                .indirect_dispatch
                .and_then(|m| m.get(&site_pc24))
                .filter(|site| site.rts_stack)
            {
                if let Some(entries) = resolve_indirect_dispatch_targets(
                    rom,
                    mapping,
                    bank,
                    &insn,
                    site.count,
                    &site.table_bases,
                    &site.targets,
                    reloc,
                ) {
                    insn.dispatch_entries = Some(entries.clone());
                    insn.dispatch_kind = Some("short".to_string());
                    insn.dispatch_idx_reg = Some('X');
                    insn.dispatch_terminal = true;
                    insn.dispatch_local_goto = true;
                    insn.dispatch_stack_pointer = true;
                    let site_m = insn.m_flag & 1;
                    let site_x = insn.x_flag & 1;
                    let mut labeled_succ = Vec::new();
                    for &entry in &entries {
                        if entry == 0 {
                            continue;
                        }
                        let eb = (entry >> 16) & 0xFF;
                        let e16 = entry & 0xFFFF;
                        if eb == bank
                            && ((0x8000..=0xFFFF).contains(&e16)
                                || addr_in_reloc_region(eb, e16, reloc).is_some())
                        {
                            labeled_succ.push((
                                DecodeKey::new(addr24(eb, e16), site_m, site_x),
                                "dispatch",
                            ));
                        }
                    }
                    let successors = labeled_succ.iter().map(|(key, _)| key.clone()).collect();
                    graph.insert(DecodedInsn {
                        key,
                        insn,
                        successors,
                    });
                    for (successor, kind) in labeled_succ {
                        if !graph.contains(&successor) {
                            worklist.push((successor, kind, pc as i64));
                        }
                    }
                    continue;
                }
            }
        }

        // cfg indirect_dispatch for RTS-stack PHA dispatchers.
        if insn.mnem == "PHA" {
            let site_pc24 = (bank << 16) | pc;
            if let Some(site) = env.indirect_dispatch.and_then(|m| m.get(&site_pc24)) {
                if let Some(entries) = resolve_indirect_dispatch_targets(
                    rom,
                    mapping,
                    bank,
                    &insn,
                    site.count,
                    &site.table_bases,
                    &site.targets,
                    reloc,
                ) {
                    insn.dispatch_entries = Some(entries.clone());
                    insn.dispatch_kind = Some(
                        if is_long_dispatch(&insn, &site.table_bases) {
                            "long"
                        } else {
                            "short"
                        }
                        .to_string(),
                    );
                    insn.dispatch_idx_reg = Some(site.idx_reg);
                    insn.dispatch_table_bases = site.table_bases.clone();
                    insn.dispatch_terminal = true;
                    let mut labeled_succ: Vec<(DecodeKey, &'static str)> = Vec::new();
                    for &e in &entries {
                        if e == 0 {
                            continue;
                        }
                        let eb = (e >> 16) & 0xFF;
                        let e16 = e & 0xFFFF;
                        if eb == bank
                            && ((0x8000..=0xFFFF).contains(&e16)
                                || addr_in_reloc_region(eb, e16, reloc).is_some())
                        {
                            labeled_succ.push((DecodeKey::new(addr24(eb, e16), 1, 1), "jump"));
                        }
                    }
                    let succ: Vec<DecodeKey> =
                        labeled_succ.iter().map(|(k, _)| k.clone()).collect();
                    graph.insert(DecodedInsn {
                        key,
                        insn,
                        successors: succ,
                    });
                    for (s, sk) in labeled_succ {
                        if !graph.contains(&s) {
                            worklist.push((s, sk, pc as i64));
                        }
                    }
                    continue;
                }
            }
        }

        // cfg-required-dispatch-or-kill for JSR (abs,X).
        if insn.mnem == "JSR" && insn.mode == Mode::IndirX {
            let site_pc24 = (bank << 16) | pc;
            let mut ud_auth: Option<Auth> = env
                .indirect_dispatch
                .and_then(|m| m.get(&site_pc24))
                .map(|s| Auth {
                    count: s.count,
                    idx_reg: s.idx_reg,
                    table_bases: s.table_bases.clone(),
                    ptr_call: s.ptr_call,
                    pointer_match: s.pointer_match,
                    popped_call_frame: s.popped_call_frame,
                    targets: s.targets.clone(),
                    local_goto: false,
                });
            if ud_auth.is_none() {
                if let Some(entries) = autorecover_indirect_xtable(
                    rom,
                    mapping,
                    bank,
                    &insn,
                    data_regions,
                    reloc,
                    start,
                ) {
                    ud_auth = Some(Auth {
                        count: entries.len() as u32,
                        idx_reg: 'X',
                        table_bases: vec![],
                        ptr_call: false,
                        pointer_match: false,
                        popped_call_frame: false,
                        // Preserve tolerated null slots; re-reading only the
                        // count would turn raw $0000 into bank:$0000.
                        targets: entries,
                        local_goto: false,
                    });
                }
            }
            if let Some(ud) = &ud_auth {
                if let Some(entries) = resolve_indirect_dispatch_targets(
                    rom,
                    mapping,
                    bank,
                    &insn,
                    ud.count,
                    &ud.table_bases,
                    &ud.targets,
                    reloc,
                ) {
                    insn.dispatch_entries = Some(entries.clone());
                    insn.dispatch_kind = Some(
                        if is_long_dispatch(&insn, &ud.table_bases) {
                            "long"
                        } else {
                            "short"
                        }
                        .to_string(),
                    );
                    insn.dispatch_idx_reg = Some(ud.idx_reg);
                    insn.dispatch_call = true;
                    insn.dispatch_pointer_match = ud.ptr_call;
                    insn.dispatch_table_bases = if ud.ptr_call {
                        vec![insn.operand & 0xFFFF]
                    } else {
                        ud.table_bases.clone()
                    };
                    let site_m = insn.m_flag & 1;
                    let site_x = insn.x_flag & 1;
                    let kind = insn.dispatch_kind.as_deref();
                    let mut return_modes = HashSet::new();
                    let mut missing_targets = Vec::new();
                    for &e in &entries {
                        if e == 0 {
                            continue;
                        }
                        let target_pc24 = if kind == Some("long") {
                            e & 0xFFFFFF
                        } else {
                            addr24(bank, e & 0xFFFF)
                        };
                        match lookup_exit_mx_modes(
                            env.callee_exit_mx,
                            env.callee_exit_mx_modes,
                            target_pc24,
                            site_m,
                            site_x,
                        ) {
                            Some(modes) => return_modes.extend(modes),
                            None => missing_targets.push(target_pc24),
                        }
                    }
                    let labeled_succ =
                        if !missing_targets.is_empty() && env.stop_on_unknown_callee_exit {
                            for target_pc24 in missing_targets {
                                let item = (insn.addr & 0xFFFFFF, target_pc24, site_m, site_x);
                                if !graph.unknown_callee_exit_sites.contains(&item) {
                                    graph.unknown_callee_exit_sites.push(item);
                                }
                            }
                            Vec::new()
                        } else {
                            if !missing_targets.is_empty() || return_modes.is_empty() {
                                return_modes.insert((site_m, site_x));
                            }
                            let mut modes: Vec<_> = return_modes.into_iter().collect();
                            modes.sort();
                            let return_pc = (pc + insn.length as u32) & 0xFFFF;
                            modes
                                .into_iter()
                                .map(|(m, x)| {
                                    (
                                        DecodeKey::with_stack(
                                            addr24(bank, return_pc),
                                            m & 1,
                                            x & 1,
                                            key.p_stack.clone(),
                                        ),
                                        "fall",
                                    )
                                })
                                .collect()
                        };
                    let succ: Vec<DecodeKey> = labeled_succ
                        .iter()
                        .map(|(successor, _)| successor.clone())
                        .collect();
                    graph.insert(DecodedInsn {
                        key,
                        insn,
                        successors: succ,
                    });
                    for (s, sk) in labeled_succ {
                        if !graph.contains(&s) {
                            worklist.push((s, sk, pc as i64));
                        }
                    }
                    continue;
                }
            }
            // Legacy indirect_call_tables.
            if let Some(auth) = env.indirect_call_tables.and_then(|m| m.get(&site_pc24)) {
                let base = auth.base & 0xFFFF;
                let count = auth.count;
                let kind = auth.kind.clone();
                let entry_size: u32 = if kind == "long" { 3 } else { 2 };
                let mut entries: Vec<u32> = Vec::new();
                let mut tbl_pc = base;
                for _ in 0..count {
                    if tbl_pc + entry_size - 1 > 0xFFFF {
                        break;
                    }
                    let tbl_off = match try_rom_offset(mapping, bank, tbl_pc, reloc) {
                        Some(o) => o,
                        None => break,
                    };
                    if tbl_off + (entry_size as usize) > rom.len() {
                        break;
                    }
                    let addr16 = rom[tbl_off] as u32 | ((rom[tbl_off + 1] as u32) << 8);
                    if kind == "long" {
                        let eb = rom[tbl_off + 2] as u32;
                        entries.push((eb << 16) | addr16);
                    } else {
                        entries.push(addr16);
                    }
                    tbl_pc += entry_size;
                }
                insn.dispatch_entries = Some(entries.clone());
                insn.dispatch_kind = Some(kind.clone());
                let mut labeled_succ = labeled_successors(
                    &insn,
                    &key,
                    bank,
                    env,
                    rom,
                    &mut graph.unknown_callee_exit_sites,
                );
                let site_m = insn.m_flag & 1;
                let site_x = insn.x_flag & 1;
                for &e in &entries {
                    let e16 = e & 0xFFFF;
                    let eb = if kind == "long" {
                        (e >> 16) & 0xFF
                    } else {
                        bank
                    };
                    if eb == bank && (0x8000..=0xFFFF).contains(&e16) {
                        labeled_succ
                            .push((DecodeKey::new(addr24(eb, e16), site_m, site_x), "jump"));
                    }
                }
                let succ: Vec<DecodeKey> = labeled_succ.iter().map(|(k, _)| k.clone()).collect();
                graph.insert(DecodedInsn {
                    key,
                    insn,
                    successors: succ,
                });
                for (s, sk) in labeled_succ {
                    if !graph.contains(&s) {
                        worklist.push((s, sk, pc as i64));
                    }
                }
                continue;
            }
            // A low-RAM operand is a runtime function-pointer slot, not a ROM
            // jump table. Preserve JSR fall-through and let the emitted
            // runtime dispatcher resolve the target. ROM/register-range
            // operands remain suppressed below as likely phantom decodes.
            if insn.operand & 0xFFFF < 0x2000 {
                insn.dispatch_runtime = true;
                insn.dispatch_idx_reg = Some('X');
                let labeled_succ = labeled_successors(
                    &insn,
                    &key,
                    bank,
                    env,
                    rom,
                    &mut graph.unknown_callee_exit_sites,
                );
                let succ = labeled_succ.iter().map(|(key, _)| key.clone()).collect();
                graph.insert(DecodedInsn {
                    key,
                    insn,
                    successors: succ,
                });
                for (successor, kind) in labeled_succ {
                    if !graph.contains(&successor) {
                        worklist.push((successor, kind, pc as i64));
                    }
                }
                continue;
            }
            // Unauthorised: drop fall-through; record for build report.
            let table_base = insn.operand & 0xFFFF;
            let (km, kx) = (key.m, key.x);
            graph.insert(DecodedInsn {
                key,
                insn,
                successors: vec![],
            });
            graph
                .suppressed_indirect_calls
                .push(SuppressedIndirectCall {
                    site_pc24,
                    table_base,
                    function_entry_pc24: addr24(bank, start),
                    entry_m: km,
                    entry_x: kx,
                });
            continue;
        }

        // Default: linear / branch / call successors.
        let labeled_succ = labeled_successors(
            &insn,
            &key,
            bank,
            env,
            rom,
            &mut graph.unknown_callee_exit_sites,
        );
        let succ: Vec<DecodeKey> = labeled_succ.iter().map(|(k, _)| k.clone()).collect();
        graph.insert(DecodedInsn {
            key,
            insn,
            successors: succ,
        });
        for (s, sk) in labeled_succ {
            if !graph.contains(&s) {
                worklist.push((s, sk, pc as i64));
            }
        }
    }

    dedupe_by_pcmx(&mut graph);
    apply_constant_z_fold(&mut graph);
    graph
}

/// Collapse DecodeKeys at the same (pc, m, x) — different p_stack — into one
/// canonical key. Port of `_dedupe_by_pcmx`.
fn dedupe_by_pcmx(graph: &mut FunctionDecodeGraph) {
    let mut canonical: HashMap<(u32, u8, u8), DecodeKey> = HashMap::new();
    let mut remap: HashMap<DecodeKey, DecodeKey> = HashMap::new();
    for di in graph.insns() {
        let pcmx = (di.key.pc, di.key.m, di.key.x);
        let ck = canonical
            .entry(pcmx)
            .or_insert_with(|| di.key.clone())
            .clone();
        remap.insert(di.key.clone(), ck);
    }

    let mut merged: Vec<DecodedInsn> = Vec::new();
    let mut merged_index: HashMap<DecodeKey, usize> = HashMap::new();
    let mut seen_succ: HashMap<DecodeKey, HashSet<DecodeKey>> = HashMap::new();
    for di in graph.insns() {
        let ck = remap[&di.key].clone();
        if let Some(&idx) = merged_index.get(&ck) {
            for s in &di.successors {
                let ms = remap.get(s).cloned().unwrap_or_else(|| s.clone());
                let set = seen_succ.get_mut(&ck).unwrap();
                if !set.contains(&ms) {
                    merged[idx].successors.push(ms.clone());
                    set.insert(ms);
                }
            }
        } else {
            let remapped_first: Vec<DecodeKey> = di
                .successors
                .iter()
                .map(|s| remap.get(s).cloned().unwrap_or_else(|| s.clone()))
                .collect();
            let set: HashSet<DecodeKey> = remapped_first.iter().cloned().collect();
            merged_index.insert(ck.clone(), merged.len());
            seen_succ.insert(ck.clone(), set);
            merged.push(DecodedInsn {
                key: ck.clone(),
                insn: di.insn.clone(),
                successors: remapped_first,
            });
        }
    }

    graph.insns_vec = merged;
    graph.index = graph
        .insns_vec
        .iter()
        .enumerate()
        .map(|(i, di)| (di.key.clone(), i))
        .collect();
    if let Some(e) = &graph.entry {
        if let Some(ck) = remap.get(e) {
            graph.entry = Some(ck.clone());
        }
    }
}

/// Constant-Z branch fold + reachability prune. Port of `_apply_constant_z_fold`.
fn apply_constant_z_fold(graph: &mut FunctionDecodeGraph) {
    if graph.is_empty() {
        return;
    }

    let mut preds: HashMap<DecodeKey, HashSet<DecodeKey>> = HashMap::new();
    for di in graph.insns() {
        for s in &di.successors {
            preds.entry(s.clone()).or_default().insert(di.key.clone());
        }
    }

    let keys: Vec<DecodeKey> = graph.insns().iter().map(|d| d.key.clone()).collect();
    for k in keys {
        let di = match graph.get(&k) {
            Some(d) => d.clone(),
            None => continue,
        };
        let insn = &di.insn;
        if insn.mnem != "BEQ" && insn.mnem != "BNE" {
            continue;
        }
        let my_preds = preds.get(&k).cloned().unwrap_or_default();
        if my_preds.len() != 1 {
            continue;
        }
        let pred_key = my_preds.iter().next().unwrap().clone();
        let pred_di = match graph.get(&pred_key) {
            Some(d) => d.clone(),
            None => continue,
        };
        let pred_insn = &pred_di.insn;
        if pred_insn.mnem != "LDA" && pred_insn.mnem != "LDX" && pred_insn.mnem != "LDY" {
            continue;
        }
        if pred_insn.mode != Mode::Imm {
            continue;
        }
        if pred_di.successors.len() != 1 || pred_di.successors[0] != k {
            continue;
        }
        if di.successors.len() != 2 {
            continue;
        }

        let width_bits: u8 = if pred_insn.mnem == "LDA" {
            if pred_insn.m_flag == 1 {
                8
            } else {
                16
            }
        } else if pred_insn.x_flag == 1 {
            8
        } else {
            16
        };
        let mask: u32 = if width_bits == 16 { 0xFFFF } else { 0xFF };
        let masked = pred_insn.operand & mask;
        let z: u8 = if masked == 0 { 1 } else { 0 };

        let fall_succ = di.successors[0].clone();
        let jump_succ = di.successors[1].clone();
        let taken = if insn.mnem == "BEQ" { z == 1 } else { z == 0 };
        let live = if taken {
            jump_succ.clone()
        } else {
            fall_succ.clone()
        };
        let dead = if taken {
            fall_succ.clone()
        } else {
            jump_succ.clone()
        };

        let mut new_insn = insn.clone();
        new_insn.const_z_fold_unconditional = true;
        new_insn.const_z_fold_dead_pc24 = Some(dead.pc & 0xFFFFFF);
        let branch_pc24 = insn.addr & 0xFFFFFF;
        let prev_pc24 = pred_insn.addr & 0xFFFFFF;
        let branch_mnem = insn.mnem.to_string();
        let prev_mnem = pred_insn.mnem.to_string();
        if let Some(&idx) = graph.index.get(&k) {
            graph.insns_vec[idx] = DecodedInsn {
                key: k.clone(),
                insn: new_insn,
                successors: vec![live.clone()],
            };
        }

        let (entry_m, entry_x) = graph.entry.as_ref().map(|e| (e.m, e.x)).unwrap_or((0, 0));
        let func_entry_pc24 = graph.entry.as_ref().map(|e| e.pc & 0xFFFFFF).unwrap_or(0);
        graph.const_z_folds.push(ConstZFold {
            branch_pc24,
            prev_pc24,
            branch_mnem,
            prev_mnem,
            prev_imm: masked,
            width_bits,
            z_value: z,
            taken_kind: if taken { "jump" } else { "fall" }.to_string(),
            live_pc24: live.pc & 0xFFFFFF,
            dead_pc24: dead.pc & 0xFFFFFF,
            func_entry_pc24,
            entry_m,
            entry_x,
        });
    }

    // Reachability prune from entry.
    let mut reachable: HashSet<DecodeKey> = HashSet::new();
    let mut work: Vec<DecodeKey> = Vec::new();
    if let Some(e) = &graph.entry {
        work.push(e.clone());
    }
    while let Some(cur) = work.pop() {
        if reachable.contains(&cur) {
            continue;
        }
        if !graph.contains(&cur) {
            continue;
        }
        reachable.insert(cur.clone());
        if let Some(d) = graph.get(&cur) {
            for s in &d.successors {
                work.push(s.clone());
            }
        }
    }
    let kept: Vec<DecodedInsn> = graph
        .insns_vec
        .drain(..)
        .filter(|d| reachable.contains(&d.key))
        .collect();
    graph.insns_vec = kept;
    graph.index = graph
        .insns_vec
        .iter()
        .enumerate()
        .map(|(i, d)| (d.key.clone(), i))
        .collect();
}

fn direct_tail_exit_keys(graph: &FunctionDecodeGraph, di: &DecodedInsn) -> Vec<(u32, u8, u8)> {
    let ins = &di.insn;
    if !matches!(ins.mnem, "JMP" | "BRA" | "BRL") {
        return Vec::new();
    }
    let outside: Vec<_> = di
        .successors
        .iter()
        .filter(|successor| !graph.contains(successor))
        .map(|successor| (successor.pc & 0xFFFFFF, successor.m & 1, successor.x & 1))
        .collect();
    if !outside.is_empty() {
        return outside;
    }
    if ins.mnem == "JMP" && ins.length == 4 && ins.dispatch_entries.is_none() {
        return vec![(ins.operand & 0xFFFFFF, ins.m_flag & 1, ins.x_flag & 1)];
    }
    Vec::new()
}

/// Prove the narrow `TSC; STA scratch ... LDA scratch; TCS` entry-stack
/// restore idiom used by DKC2's 3D background builder.
fn entry_stack_restore_tcs_keys(graph: &FunctionDecodeGraph) -> HashSet<DecodeKey> {
    let Some(entry) = graph.entry.as_ref() else {
        return HashSet::new();
    };
    let Some(entry_di) = graph.get(entry) else {
        return HashSet::new();
    };
    if entry_di.insn.mnem != "TSC" || entry_di.successors.len() != 1 {
        return HashSet::new();
    }
    let save_key = &entry_di.successors[0];
    let Some(save_di) = graph.get(save_key) else {
        return HashSet::new();
    };
    let save = &save_di.insn;
    if save.mnem != "STA" || (save.m_flag & 1) != 0 || save.mode == Mode::Imm {
        return HashSet::new();
    }
    let scratch = save.operand & 0xFFFFFF;
    let scratch_mode = save.mode;
    let is_writer = |mnem: &str| {
        matches!(
            mnem,
            "STA"
                | "STX"
                | "STY"
                | "STZ"
                | "INC"
                | "DEC"
                | "ASL"
                | "LSR"
                | "ROL"
                | "ROR"
                | "TRB"
                | "TSB"
        )
    };
    for di in graph.insns() {
        let ins = &di.insn;
        if matches!(ins.mnem, "JSR" | "JSL" | "PLD" | "TCD") {
            return HashSet::new();
        }
        if is_writer(ins.mnem)
            && ins.mode == scratch_mode
            && (ins.operand & 0xFFFFFF) == scratch
            && &di.key != save_key
        {
            return HashSet::new();
        }
    }

    let mut predecessors: HashMap<DecodeKey, HashSet<DecodeKey>> = HashMap::new();
    for di in graph.insns() {
        for successor in &di.successors {
            if graph.contains(successor) {
                predecessors
                    .entry(successor.clone())
                    .or_default()
                    .insert(di.key.clone());
            }
        }
    }

    let mut restores = HashSet::new();
    for di in graph.insns() {
        if di.insn.mnem != "TCS" {
            continue;
        }
        let Some(preds) = predecessors.get(&di.key) else {
            continue;
        };
        if preds.len() != 1 {
            continue;
        }
        let Some(load_di) = graph.get(preds.iter().next().unwrap()) else {
            continue;
        };
        let load = &load_di.insn;
        if load.mnem == "LDA"
            && (load.m_flag & 1) == 0
            && load.mode == scratch_mode
            && (load.operand & 0xFFFFFF) == scratch
        {
            restores.insert(di.key.clone());
        }
    }
    restores
}

/// Local guest-stack deltas reaching each architectural return instruction.
/// `None` is an indeterminate height after TCS/TXS.
fn return_stack_delta_states(graph: &FunctionDecodeGraph) -> HashMap<u32, HashSet<Option<i32>>> {
    fn local_delta(ins: &Insn) -> Option<i32> {
        if ins.dispatch_consumed_stack_bytes != 0 {
            return Some(-(ins.dispatch_consumed_stack_bytes as i32));
        }
        if ins.mnem == "PEI" && ins.dispatch_stack_pointer {
            return Some(0);
        }
        match ins.mnem {
            "PEA" | "PEI" | "PER" | "PHD" => Some(2),
            "PHP" | "PHB" | "PHK" => Some(1),
            "PHA" => Some(if (ins.m_flag & 1) != 0 { 1 } else { 2 }),
            "PHX" | "PHY" => Some(if (ins.x_flag & 1) != 0 { 1 } else { 2 }),
            "PLD" => Some(-2),
            "PLP" | "PLB" => Some(-1),
            "PLA" => Some(if (ins.m_flag & 1) != 0 { -1 } else { -2 }),
            "PLX" | "PLY" => Some(if (ins.x_flag & 1) != 0 { -1 } else { -2 }),
            "TCS" | "TXS" => None,
            _ => Some(0),
        }
    }

    let restoring_tcs = entry_stack_restore_tcs_keys(graph);
    let Some(entry) = graph.entry.as_ref() else {
        return HashMap::new();
    };
    let mut in_states: HashMap<DecodeKey, HashSet<Option<i32>>> =
        HashMap::from([(entry.clone(), HashSet::from([Some(0)]))]);
    let mut worklist = vec![entry.clone()];
    let mut returns: HashMap<u32, HashSet<Option<i32>>> = HashMap::new();
    while let Some(key) = worklist.pop() {
        let Some(di) = graph.get(&key) else {
            continue;
        };
        let states = in_states.get(&key).cloned().unwrap_or_default();
        if states.is_empty() {
            continue;
        }
        let ins = &di.insn;
        if matches!(ins.mnem, "RTS" | "RTL" | "RTI") {
            returns
                .entry(ins.addr & 0xFFFFFF)
                .or_default()
                .extend(states);
            continue;
        }
        let mut next_states = HashSet::new();
        if restoring_tcs.contains(&key) {
            next_states.insert(Some(0));
        } else {
            let delta = local_delta(ins);
            for state in states {
                next_states.insert(match (state, delta) {
                    (Some(value), Some(change)) => Some((value + change).clamp(-64, 64)),
                    _ => None,
                });
            }
        }
        for successor in &di.successors {
            if !graph.contains(successor) {
                continue;
            }
            let old = in_states.get(successor).cloned().unwrap_or_default();
            let mut merged = old.clone();
            merged.extend(next_states.iter().copied());
            if merged != old {
                in_states.insert(successor.clone(), merged);
                worklist.push(successor.clone());
            }
        }
    }
    returns
}

/// Deterministic structured diagnostic for the env-gated solver snapshot.
pub fn function_return_stack_delta_states(
    graph: &FunctionDecodeGraph,
) -> std::collections::BTreeMap<u32, Vec<Option<i32>>> {
    return_stack_delta_states(graph)
        .into_iter()
        .map(|(pc24, states)| {
            let mut states: Vec<_> = states.into_iter().collect();
            states.sort();
            (pc24, states)
        })
        .collect()
}

fn return_frame_size(ins: &Insn) -> Option<i32> {
    match ins.mnem {
        "RTS" => Some(2),
        "RTL" => Some(3),
        _ => None,
    }
}

fn return_site_is_partial_nlr(
    ins: &Insn,
    states_by_pc: &HashMap<u32, HashSet<Option<i32>>>,
) -> bool {
    let Some(frame) = return_frame_size(ins) else {
        return false;
    };
    let Some(states) = states_by_pc.get(&(ins.addr & 0xFFFFFF)) else {
        return false;
    };
    !states.is_empty()
        && states
            .iter()
            .all(|state| matches!(state, Some(value) if *value > 0 && *value < frame))
}

fn has_unproven_nonlocal_return(
    graph: &FunctionDecodeGraph,
    states_by_pc: &HashMap<u32, HashSet<Option<i32>>>,
) -> bool {
    for di in graph.insns() {
        let ins = &di.insn;
        if !matches!(ins.mnem, "RTS" | "RTL" | "RTI") {
            continue;
        }
        let frame = return_frame_size(ins);
        for state in states_by_pc
            .get(&(ins.addr & 0xFFFFFF))
            .into_iter()
            .flatten()
        {
            match state {
                None => return true,
                Some(value) if *value > 0 && frame.is_none_or(|size| *value >= size) => {
                    return true;
                }
                _ => {}
            }
        }
    }
    false
}

/// Return this graph's local exits and exact tail/dispatch exit dependencies.
/// This equation form lets the whole-program analyzer prove a closed recursive
/// component without guessing that any callee preserves M/X.
pub fn function_exit_mx_equation(
    graph: &FunctionDecodeGraph,
) -> (HashSet<(u8, u8)>, HashSet<(u32, u8, u8)>) {
    let mut local_modes = HashSet::new();
    let mut dependencies = HashSet::new();
    let return_states = return_stack_delta_states(graph);
    if has_unproven_nonlocal_return(graph, &return_states) {
        dependencies.insert((0xFFFFFFFF, 0, 0));
    }

    for di in graph.insns() {
        let ins = &di.insn;
        if matches!(ins.mnem, "RTS" | "RTL" | "RTI") {
            if return_site_is_partial_nlr(ins, &return_states) {
                continue;
            }
            local_modes.insert((ins.m_flag & 1, ins.x_flag & 1));
            continue;
        }
        let tail_keys = direct_tail_exit_keys(graph, di);
        if !tail_keys.is_empty() {
            dependencies.extend(
                tail_keys
                    .into_iter()
                    .map(|(target, m, x)| (target & 0xFFFFFF, m & 1, x & 1)),
            );
            continue;
        }
        let is_dispatch_term = ins.dispatch_entries.is_some()
            && di.successors.is_empty()
            && matches!(ins.mnem, "JSL" | "JMP");
        if !is_dispatch_term {
            continue;
        }
        let site_m = ins.m_flag & 1;
        let site_x = ins.x_flag & 1;
        let dispatcher_bank = (ins.addr >> 16) & 0xFF;
        let kind = ins.dispatch_kind.as_deref();
        for &entry in ins.dispatch_entries.as_deref().unwrap_or(&[]) {
            if entry == 0 {
                continue;
            }
            let target = if kind == Some("long") {
                entry & 0xFFFFFF
            } else {
                (dispatcher_bank << 16) | (entry & 0xFFFF)
            };
            dependencies.insert((target, site_m, site_x));
        }
    }
    for (_, target) in &graph.boundary_exits {
        dependencies.insert((target.pc & 0xFFFFFF, target.m & 1, target.x & 1));
    }
    (local_modes, dependencies)
}

fn lookup_exit_mx(
    callee_exit_mx: Option<&HashMap<(u32, u8, u8), (u8, u8)>>,
    pc24: u32,
    m: u8,
    x: u8,
) -> Option<(u8, u8)> {
    let map = callee_exit_mx?;
    let key = (pc24 & 0xFFFFFF, m & 1, x & 1);
    map.get(&key).copied().or_else(|| {
        let bank = (pc24 >> 16) & 0xFF;
        if bank < 0x40 || (0x80..0xC0).contains(&bank) {
            map.get(&((pc24 ^ 0x800000) & 0xFFFFFF, m & 1, x & 1))
                .copied()
        } else {
            None
        }
    })
}

fn lookup_exit_mx_modes(
    callee_exit_mx: Option<&HashMap<(u32, u8, u8), (u8, u8)>>,
    callee_exit_mx_modes: Option<&HashMap<(u32, u8, u8), Vec<(u8, u8)>>>,
    pc24: u32,
    m: u8,
    x: u8,
) -> Option<Vec<(u8, u8)>> {
    if let Some(exact) = lookup_exit_mx(callee_exit_mx, pc24, m, x) {
        return Some(vec![(exact.0 & 1, exact.1 & 1)]);
    }
    let map = callee_exit_mx_modes?;
    let key = (pc24 & 0xFFFFFF, m & 1, x & 1);
    let mut result = map.get(&key).cloned().or_else(|| {
        let bank = (pc24 >> 16) & 0xFF;
        if bank < 0x40 || (0x80..0xC0).contains(&bank) {
            map.get(&((pc24 ^ 0x800000) & 0xFFFFFF, m & 1, x & 1))
                .cloned()
        } else {
            None
        }
    })?;
    result
        .iter_mut()
        .for_each(|pair| *pair = (pair.0 & 1, pair.1 & 1));
    result.sort();
    result.dedup();
    Some(result)
}

/// Exit (m, x) meet across a function's return paths; (None, None) if ambiguous.
/// Port of `analyze_function_exit_mx`.
pub fn analyze_function_exit_mx(
    graph: &FunctionDecodeGraph,
    callee_exit_mx: Option<&HashMap<(u32, u8, u8), (u8, u8)>>,
) -> (Option<u8>, Option<u8>) {
    fn accumulate(
        em: u8,
        ex: u8,
        exit_m: &mut Option<u8>,
        exit_x: &mut Option<u8>,
        have_any: &mut bool,
        m_ambig: &mut bool,
        x_ambig: &mut bool,
    ) {
        if !*have_any {
            *exit_m = Some(em);
            *exit_x = Some(ex);
            *have_any = true;
            return;
        }
        if !*m_ambig && *exit_m != Some(em) {
            *m_ambig = true;
        }
        if !*x_ambig && *exit_x != Some(ex) {
            *x_ambig = true;
        }
    }

    let return_states = return_stack_delta_states(graph);
    if has_unproven_nonlocal_return(graph, &return_states) {
        return (None, None);
    }

    let mut exit_m: Option<u8> = None;
    let mut exit_x: Option<u8> = None;
    let mut have_any = false;
    let mut m_ambig = false;
    let mut x_ambig = false;

    for di in graph.insns() {
        let ins = &di.insn;
        if ins.mnem == "RTS" || ins.mnem == "RTL" || ins.mnem == "RTI" {
            if return_site_is_partial_nlr(ins, &return_states) {
                continue;
            }
            accumulate(
                ins.m_flag & 1,
                ins.x_flag & 1,
                &mut exit_m,
                &mut exit_x,
                &mut have_any,
                &mut m_ambig,
                &mut x_ambig,
            );
            continue;
        }
        let tail_keys = direct_tail_exit_keys(graph, di);
        if !tail_keys.is_empty() {
            for (target, site_m, site_x) in tail_keys {
                let Some((em, ex)) = lookup_exit_mx(callee_exit_mx, target, site_m, site_x) else {
                    return (None, None);
                };
                accumulate(
                    em & 1,
                    ex & 1,
                    &mut exit_m,
                    &mut exit_x,
                    &mut have_any,
                    &mut m_ambig,
                    &mut x_ambig,
                );
            }
            continue;
        }
        let is_dispatch_term = ins.dispatch_entries.is_some()
            && di.successors.is_empty()
            && (ins.mnem == "JSL" || ins.mnem == "JMP");
        if is_dispatch_term {
            let cem = match callee_exit_mx {
                Some(c) => c,
                None => return (None, None),
            };
            let site_m = ins.m_flag & 1;
            let site_x = ins.x_flag & 1;
            let dispatcher_bank = (ins.addr >> 16) & 0xFF;
            let kind = ins.dispatch_kind.as_deref();
            for &entry in ins.dispatch_entries.as_deref().unwrap_or(&[]) {
                if entry == 0 {
                    continue;
                }
                let tgt_pc24 = if kind == Some("long") {
                    entry & 0xFFFFFF
                } else {
                    (dispatcher_bank << 16) | (entry & 0xFFFF)
                };
                match cem.get(&(tgt_pc24, site_m, site_x)) {
                    None => return (None, None),
                    Some(&(hm, hx)) => accumulate(
                        hm & 1,
                        hx & 1,
                        &mut exit_m,
                        &mut exit_x,
                        &mut have_any,
                        &mut m_ambig,
                        &mut x_ambig,
                    ),
                }
            }
        }
    }

    for (_, target) in &graph.boundary_exits {
        let Some((em, ex)) = lookup_exit_mx(callee_exit_mx, target.pc, target.m, target.x) else {
            return (None, None);
        };
        accumulate(
            em & 1,
            ex & 1,
            &mut exit_m,
            &mut exit_x,
            &mut have_any,
            &mut m_ambig,
            &mut x_ambig,
        );
    }

    if m_ambig {
        exit_m = None;
    }
    if x_ambig {
        exit_x = None;
    }
    if !have_any {
        return (None, None);
    }
    (exit_m, exit_x)
}

/// Concrete set of (m, x) states at exits, or None. Port of
/// `analyze_function_exit_mx_modes`.
pub fn analyze_function_exit_mx_modes(
    graph: &FunctionDecodeGraph,
    callee_exit_mx: Option<&HashMap<(u32, u8, u8), (u8, u8)>>,
) -> Option<Vec<(u8, u8)>> {
    analyze_function_exit_mx_modes_with_sets(graph, callee_exit_mx, None)
}

/// Concrete exit states with exact and multi-mode callee facts composed
/// through direct tail chains and dispatch terminators.
pub fn analyze_function_exit_mx_modes_with_sets(
    graph: &FunctionDecodeGraph,
    callee_exit_mx: Option<&HashMap<(u32, u8, u8), (u8, u8)>>,
    callee_exit_mx_modes: Option<&HashMap<(u32, u8, u8), Vec<(u8, u8)>>>,
) -> Option<Vec<(u8, u8)>> {
    let return_states = return_stack_delta_states(graph);
    if has_unproven_nonlocal_return(graph, &return_states) {
        return None;
    }
    let mut modes: HashSet<(u8, u8)> = HashSet::new();
    for di in graph.insns() {
        let ins = &di.insn;
        if ins.mnem == "RTS" || ins.mnem == "RTL" || ins.mnem == "RTI" {
            if return_site_is_partial_nlr(ins, &return_states) {
                continue;
            }
            modes.insert((ins.m_flag & 1, ins.x_flag & 1));
            continue;
        }
        let tail_keys = direct_tail_exit_keys(graph, di);
        if !tail_keys.is_empty() {
            for (target, site_m, site_x) in tail_keys {
                modes.extend(lookup_exit_mx_modes(
                    callee_exit_mx,
                    callee_exit_mx_modes,
                    target,
                    site_m,
                    site_x,
                )?);
            }
            continue;
        }
        let is_dispatch_term = ins.dispatch_entries.is_some()
            && di.successors.is_empty()
            && (ins.mnem == "JSL" || ins.mnem == "JMP");
        if is_dispatch_term {
            let site_m = ins.m_flag & 1;
            let site_x = ins.x_flag & 1;
            let dispatcher_bank = (ins.addr >> 16) & 0xFF;
            let kind = ins.dispatch_kind.as_deref();
            for &entry in ins.dispatch_entries.as_deref().unwrap_or(&[]) {
                if entry == 0 {
                    continue;
                }
                let tgt_pc24 = if kind == Some("long") {
                    entry & 0xFFFFFF
                } else {
                    (dispatcher_bank << 16) | (entry & 0xFFFF)
                };
                modes.extend(lookup_exit_mx_modes(
                    callee_exit_mx,
                    callee_exit_mx_modes,
                    tgt_pc24,
                    site_m,
                    site_x,
                )?);
            }
        }
    }

    for (_, target) in &graph.boundary_exits {
        modes.extend(lookup_exit_mx_modes(
            callee_exit_mx,
            callee_exit_mx_modes,
            target.pc,
            target.m,
            target.x,
        )?);
    }
    if !graph.unresolved_indirects.is_empty()
        || !graph.suppressed_indirect_calls.is_empty()
        || !graph.unknown_callee_exit_sites.is_empty()
    {
        return None;
    }
    // Empty is a complete proof for a closed graph with no architectural
    // return path; None is reserved for unresolved dependencies.
    let mut v: Vec<(u8, u8)> = modes.into_iter().collect();
    v.sort();
    Some(v)
}

/// Classify a JSL-jump-table dispatch helper: Some("short"|"long") or None.
/// Port of `classify_dispatch_helper`.
pub fn classify_dispatch_helper(
    rom: &[u8],
    mapping: RomMapping,
    bank: u32,
    addr: u32,
) -> Option<&'static str> {
    let mut insns: Vec<Insn> = Vec::new();
    let mut pc = addr & 0xFFFF;
    let mut m = 1u8;
    let mut x = 1u8;
    let mut safety = 0;
    while safety < 256 {
        safety += 1;
        if !is_rom_address(mapping, bank, pc) {
            return None;
        }
        let offset = rom_offset_opt(mapping, bank, pc)?;
        if offset >= rom.len() {
            return None;
        }
        let ins = decode_insn(rom, offset, pc, bank, m, x)?;
        let mnem = ins.mnem;
        let length = ins.length;
        if mnem == "REP" {
            if ins.operand & 0x20 != 0 {
                m = 0;
            }
            if ins.operand & 0x10 != 0 {
                x = 0;
            }
        } else if mnem == "SEP" {
            if ins.operand & 0x20 != 0 {
                m = 1;
            }
            if ins.operand & 0x10 != 0 {
                x = 1;
            }
        }
        let stop = matches!(
            mnem,
            "RTS" | "RTL" | "RTI" | "BRA" | "BRL" | "JMP" | "JML" | "STP"
        );
        insns.push(ins);
        if stop {
            break;
        }
        pc = (pc + length as u32) & 0xFFFF;
    }

    if insns.is_empty() {
        return None;
    }
    if !insns.iter().any(|i| i.mnem == "PLA" || i.mnem == "PLY") {
        return None;
    }
    let last = insns.last().unwrap();
    if !(last.mnem == "JMP" && matches!(last.mode, Mode::Indir | Mode::IndirX | Mode::IndirL)) {
        return None;
    }
    let mut asl_seen = false;
    let mut has_adc = false;
    for ins in &insns {
        if !asl_seen {
            if ins.mnem == "ASL" && ins.mode == Mode::Acc {
                asl_seen = true;
            }
            continue;
        }
        if ins.mnem == "ADC" {
            has_adc = true;
        }
        if ins.mnem == "TAY" || ins.mnem == "TAX" {
            return Some(if has_adc { "long" } else { "short" });
        }
    }
    None
}

/// Detect a subroutine that advances its stacked return address past a fixed
/// number of inline argument bytes. Port of Python's
/// `detect_inline_arg_bytes`.
///
/// This deliberately recognizes only the load-return-address, add-immediate,
/// store-back-to-the-same-stack-slot idiom. Instructions that may clobber A
/// reset the match, which keeps ordinary routines that merely inspect their
/// return address from being classified as inline-argument callees.
pub fn detect_inline_arg_bytes(
    rom: &[u8],
    mapping: RomMapping,
    bank: u32,
    addr: u32,
    entry_m: u8,
    entry_x: u8,
) -> Option<u8> {
    fn preserves_a(opcode: u8) -> bool {
        matches!(
            opcode,
            0x85 | 0x8D
                | 0x8F
                | 0x95
                | 0x9D
                | 0x99
                | 0x92
                | 0x87
                | 0x97
                | 0x81
                | 0x91
                | 0x9C
                | 0x9E
                | 0x86
                | 0x8E
                | 0x96
                | 0x84
                | 0x8C
                | 0x94
                | 0x18
                | 0x38
                | 0xD8
                | 0xF8
                | 0x58
                | 0x78
                | 0xB8
                | 0xAA
                | 0xA8
                | 0xE8
                | 0xC8
                | 0xCA
                | 0x88
                | 0xA2
                | 0xA6
                | 0xB6
                | 0xAE
                | 0xBE
                | 0xA0
                | 0xA4
                | 0xB4
                | 0xAC
                | 0xBC
                | 0xE0
                | 0xE4
                | 0xEC
                | 0xC0
                | 0xC4
                | 0xCC
                | 0x48
                | 0xDA
                | 0x5A
                | 0x08
                | 0x8B
                | 0x4B
                | 0x0B
                | 0xEA
                | 0x42
        )
    }

    fn mutates_y(opcode: u8) -> bool {
        matches!(
            opcode,
            0xA0 | 0xA4 | 0xB4 | 0xAC | 0xBC | 0xC8 | 0x88 | 0x7A
        )
    }

    let mut pc = addr & 0xFFFF;
    let mut m = entry_m & 1;
    let mut x = entry_x & 1;
    let mut a_slot: Option<u8> = None;
    let mut a_added = 0u32;
    let mut y_slot: Option<u8> = None;
    let mut y_added = 0u32;

    for _ in 0..96 {
        let offset = rom_offset_opt(mapping, bank, pc)?;
        if offset >= rom.len() {
            return None;
        }
        let ins = decode_insn(rom, offset, pc, bank, m, x)?;
        if is_terminator(ins.mnem) {
            return None;
        }

        match ins.mnem {
            "REP" => {
                if ins.operand & 0x20 != 0 {
                    m = 0;
                }
                if ins.operand & 0x10 != 0 {
                    x = 0;
                }
            }
            "SEP" => {
                if ins.operand & 0x20 != 0 {
                    m = 1;
                }
                if ins.operand & 0x10 != 0 {
                    x = 1;
                }
            }
            "LDA" if ins.mode == Mode::Stk => {
                a_slot = Some((ins.operand & 0xFF) as u8);
                a_added = 0;
            }
            "TAY" => {
                y_slot = a_slot;
                y_added = if a_slot.is_some() { a_added } else { 0 };
            }
            "TYA" => {
                a_slot = y_slot;
                a_added = if y_slot.is_some() { y_added } else { 0 };
            }
            "ADC" if ins.opcode == 0x69 && a_slot.is_some() => {
                a_added = (a_added + ins.operand) & 0xFFFF;
            }
            "STA" if ins.opcode == 0x83 && ins.mode == Mode::Stk => {
                if a_slot == Some((ins.operand & 0xFF) as u8) && a_added != 0 {
                    return Some((a_added & 0xFF) as u8);
                }
            }
            _ if preserves_a(ins.opcode) => {
                if mutates_y(ins.opcode) {
                    y_slot = None;
                    y_added = 0;
                }
            }
            _ => {
                a_slot = None;
                a_added = 0;
            }
        }
        pc = (pc + ins.length as u32) & 0xFFFF;
    }
    None
}

/// Active cartridge mapping returning `None` for addresses outside a ROM window.
fn rom_offset_opt(mapping: RomMapping, bank: u32, addr: u32) -> Option<usize> {
    is_rom_address(mapping, bank, addr).then(|| addr_to_rom_offset(mapping, bank, addr, &[]))
}

// ── Decode-dependency derivation (for the emit-output cache) ──────────────────
//
// `decode_function` is pure in (rom, bank, start, m, x, end, env). Across the
// orchestrator's ~6 auto-promote passes the same functions are re-decoded +
// re-EMITTED tens of times. The only env inputs that VARY across passes are
// `callee_exit_mx` / `callee_exit_mx_modes` (grow as routes are discovered) and
// `sibling_entry_pcs` (grows as a bank gains entries); the rest (data_regions,
// reloc, dispatch_helpers, inline_skip, indirect_dispatch, …) are fixed once.
//
// `compute_deps` extracts, from a decoded graph, the exact decode-varying inputs
// that graph consulted: every `callee_exit_mx`/`_modes` (key→value) pair (every
// JSR-len3 / JSL static call target at its call-site (m,x), plus the bank-mirror
// key — exactly the keys `labeled_successors` queries) and the set of imported
// successor PCs (for the sibling dependency). `decode_deps_match` then tests
// whether those recorded deps still hold under a later `env`; when they do,
// re-decoding would produce an identical graph. The EmitCache (in emit.rs) pairs
// this with a graph-derived EMIT-dependency set to memoize the emitted C text.
//
// SAFETY: the caller must keep the *fixed* env (everything except cem/cmm/sibling)
// constant for the cache's lifetime. The orchestrator builds the cache after
// autoroute (dispatch_helpers final) and uses it only on the emit path, which
// always passes that one consistent fixed env.

pub(crate) type CemKey = (u32, u8, u8);
pub(crate) type CemDep = (CemKey, Option<(u8, u8)>);
pub(crate) type CmmDep = (CemKey, Option<Vec<(u8, u8)>>);
type CacheBase = (u32, u32, u8, u8, Option<u32>, bool);

/// The exact `callee_exit_mx` / `_modes` keys `labeled_successors` queries (every
/// JSR-len3/JSL static call target at its call-site (m,x), plus the bank-mirror)
/// and the set of imported successor PCs (for the sibling dependency).
pub(crate) fn compute_deps(
    graph: &FunctionDecodeGraph,
    env: &DecodeEnv,
) -> (Vec<CemDep>, Vec<CmmDep>, Vec<u32>) {
    let mut cem_keys: Vec<CemKey> = Vec::new();
    let mut sib_pcs: Vec<u32> = Vec::new();
    for di in graph.insns() {
        let ins = &di.insn;
        let dispatch_targets: Vec<u32> =
            if ins.mnem == "JSR" && ins.mode == Mode::IndirX && ins.dispatch_entries.is_some() {
                let bank = (ins.addr >> 16) & 0xFF;
                ins.dispatch_entries
                    .as_deref()
                    .unwrap_or(&[])
                    .iter()
                    .copied()
                    .filter(|entry| *entry != 0)
                    .map(|entry| {
                        if ins.dispatch_kind.as_deref() == Some("long") {
                            entry & 0xFFFFFF
                        } else {
                            addr24(bank, entry & 0xFFFF)
                        }
                    })
                    .collect()
            } else {
                Vec::new()
            };
        let tp: Option<u32> = if ins.mnem == "JSR" && ins.length == 3 && ins.mode != Mode::IndirX {
            Some(addr24((ins.addr >> 16) & 0xFF, ins.operand & 0xFFFF))
        } else if ins.mnem == "JSL" {
            Some(ins.operand & 0xFFFFFF)
        } else {
            None
        };
        if let Some(tp) = tp {
            cem_keys.push((tp, ins.m_flag, ins.x_flag));
            let tbank = (tp >> 16) & 0xFF;
            if tbank < 0x40 || (0x80..0xC0).contains(&tbank) {
                cem_keys.push((tp ^ 0x800000, ins.m_flag, ins.x_flag));
            }
        }
        for tp in dispatch_targets {
            cem_keys.push((tp, ins.m_flag, ins.x_flag));
            let tbank = (tp >> 16) & 0xFF;
            if tbank < 0x40 || (0x80..0xC0).contains(&tbank) {
                cem_keys.push((tp ^ 0x800000, ins.m_flag, ins.x_flag));
            }
        }
        for s in &di.successors {
            sib_pcs.push(s.pc & 0xFFFF);
        }
    }
    cem_keys.sort_unstable();
    cem_keys.dedup();
    sib_pcs.sort_unstable();
    sib_pcs.dedup();
    let cem = env.callee_exit_mx;
    let cmm = env.callee_exit_mx_modes;
    let deps: Vec<CemDep> = cem_keys
        .iter()
        .map(|k| (*k, cem.and_then(|m| m.get(k)).copied()))
        .collect();
    let mdeps: Vec<CmmDep> = cem_keys
        .iter()
        .map(|k| (*k, cmm.and_then(|m| m.get(k)).cloned()))
        .collect();
    (deps, mdeps, sib_pcs)
}

/// True iff the recorded decode dependencies (the `compute_deps` output of a
/// previously-decoded graph) still hold under the current `env` — i.e. every
/// consulted `callee_exit_mx` / `_modes` key returns the same value and none of
/// the imported successor PCs has since become a sibling entry. When this holds,
/// re-decoding under `env` would produce an identical graph. Extracted from the
/// former `DecodeCache::get_or_decode` check so the emit-output cache can reuse it.
pub(crate) fn decode_deps_match(
    cem_deps: &[CemDep],
    cmm_deps: &[CmmDep],
    sib_deps: &[u32],
    env: &DecodeEnv,
) -> bool {
    let cem = env.callee_exit_mx;
    let cmm = env.callee_exit_mx_modes;
    let sib = env.sibling_entry_pcs;
    if !cem_deps
        .iter()
        .all(|(k, v)| cem.and_then(|m| m.get(k)).copied() == *v)
    {
        return false;
    }
    if !cmm_deps
        .iter()
        .all(|(k, v)| cmm.and_then(|m| m.get(k)).cloned() == *v)
    {
        return false;
    }
    match sib {
        None => true,
        Some(s) => !sib_deps.iter().any(|pc| s.contains(pc)),
    }
}

// ── Decode cache (for the exit-mx fixpoint + dispatch discovery) ──────────────
//
// `exit_mx_detect_and_route` runs a 12-iteration fixpoint over every named entry
// × 4 (m,x), re-decoding the same functions as `callee_exit_mx` evolves — and the
// orchestrator re-runs that whole pass once per auto-promote pass. That single
// function is ~95% of regen wall-clock. This cache memoizes those decodes,
// dependency-keyed on callee_exit_mx exactly like the EmitCache, so it stays
// correct as the fixpoint mutates callee_exit_mx (hits when a function's
// dependencies are stable across iterations/passes).
//
// SAFETY: as with the EmitCache, the FIXED env (dispatch_helpers, reloc,
// inline_skip) must be constant for the cache's lifetime. A given DecodeCache is
// therefore used by ONE caller-env profile (here: exit-mx's env) — never shared
// with the emit path, whose env differs.

#[derive(Default)]
pub struct DecodeCache {
    map: Mutex<HashMap<CacheBase, Vec<DecodeCacheEntry>>>,
    pub hits: AtomicU64,
    pub misses: AtomicU64,
}

struct DecodeCacheEntry {
    cem_deps: Vec<CemDep>,
    cmm_deps: Vec<CmmDep>,
    sib_deps: Vec<u32>,
    graph: Arc<FunctionDecodeGraph>,
}

impl DecodeCache {
    pub fn new() -> Self {
        Self::default()
    }

    /// Decode (bank,start,m,x,end) under `env`, reusing a cached graph when the
    /// recorded dependencies still hold.
    pub fn get_or_decode(
        &self,
        rom: &[u8],
        bank: u32,
        start: u32,
        m: u8,
        x: u8,
        end: Option<u32>,
        env: &DecodeEnv,
    ) -> Arc<FunctionDecodeGraph> {
        let m = m & 1;
        let x = x & 1;
        let base: CacheBase = (
            bank & 0xFF,
            start & 0xFFFF,
            m,
            x,
            end,
            env.callee_exit_mx.is_some(),
        );
        {
            let map = self.map.lock().unwrap();
            if let Some(bucket) = map.get(&base) {
                // Exit facts grow monotonically, so the newest dependency
                // snapshot is overwhelmingly the likely hit on the next
                // fixed-point round.
                for e in bucket.iter().rev() {
                    if decode_deps_match(&e.cem_deps, &e.cmm_deps, &e.sib_deps, env) {
                        self.hits.fetch_add(1, Ordering::Relaxed);
                        return e.graph.clone();
                    }
                }
            }
        }
        let g = Arc::new(decode_function(rom, bank, start, m, x, end, env));
        let (cem_deps, cmm_deps, sib_deps) = compute_deps(&g, env);
        self.misses.fetch_add(1, Ordering::Relaxed);
        self.map
            .lock()
            .unwrap()
            .entry(base)
            .or_default()
            .push(DecodeCacheEntry {
                cem_deps,
                cmm_deps,
                sib_deps,
                graph: g.clone(),
            });
        g
    }

    /// Sequential fast path for analysis.  It avoids taking the cache mutex
    /// 170k+ times while retaining the exact same dependency validation as
    /// `get_or_decode`.  Parallel emit continues to use the thread-safe API.
    pub fn get_or_decode_local(
        &mut self,
        rom: &[u8],
        bank: u32,
        start: u32,
        m: u8,
        x: u8,
        end: Option<u32>,
        env: &DecodeEnv,
    ) -> Arc<FunctionDecodeGraph> {
        let m = m & 1;
        let x = x & 1;
        let base: CacheBase = (
            bank & 0xFF,
            start & 0xFFFF,
            m,
            x,
            end,
            env.callee_exit_mx.is_some(),
        );
        {
            let map = self.map.get_mut().unwrap();
            if let Some(bucket) = map.get(&base) {
                for entry in bucket.iter().rev() {
                    if decode_deps_match(&entry.cem_deps, &entry.cmm_deps, &entry.sib_deps, env) {
                        self.hits.fetch_add(1, Ordering::Relaxed);
                        return entry.graph.clone();
                    }
                }
            }
        }
        let graph = Arc::new(decode_function(rom, bank, start, m, x, end, env));
        let (cem_deps, cmm_deps, sib_deps) = compute_deps(&graph, env);
        self.misses.fetch_add(1, Ordering::Relaxed);
        self.map
            .get_mut()
            .unwrap()
            .entry(base)
            .or_default()
            .push(DecodeCacheEntry {
                cem_deps,
                cmm_deps,
                sib_deps,
                graph: graph.clone(),
            });
        graph
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::insn::decode_insn;

    #[test]
    fn post_state_sep_rep() {
        // SEP #$30 sets both m and x.
        let data = [0xE2, 0x30];
        let i = decode_insn(&data, 0, 0x8000, 0, 0, 0).unwrap();
        let (m, x, _) = post_state(&i, 0, 0, &[]);
        assert_eq!((m, x), (1, 1));
        // REP #$20 clears m only.
        let data = [0xC2, 0x20];
        let i = decode_insn(&data, 0, 0x8000, 1, 1, 1).unwrap();
        let (m, x, _) = post_state(&i, 1, 1, &[]);
        assert_eq!((m, x), (0, 1));
    }

    /// Build a bank-0 ROM with `bytes` placed at local PC $8000.
    fn rom_at_8000(bytes: &[u8]) -> Vec<u8> {
        let mut rom = vec![0u8; 0x8000 + 0x200];
        rom[..bytes.len()].copy_from_slice(bytes);
        rom
    }

    #[test]
    fn detects_direct_inline_argument_adjustment() {
        let rom = rom_at_8000(&[
            0xC2, 0x30, // REP #$30
            0xA3, 0x01, // LDA $01,S
            0x18, // CLC
            0x69, 0x03, 0x00, // ADC #$0003
            0x83, 0x01, // STA $01,S
            0x6B, // RTL
        ]);
        assert_eq!(
            detect_inline_arg_bytes(&rom, RomMapping::LoRom, 0, 0x8000, 1, 1),
            Some(3)
        );
    }

    #[test]
    fn detects_y_carried_inline_argument_adjustment() {
        let rom = rom_at_8000(&[
            0x08, // PHP
            0x8B, // PHB
            0xC2, 0x30, // REP #$30
            0xA3, 0x04, // LDA $04,S
            0x48, // PHA
            0xAB, // PLB
            0xAB, // PLB
            0xA3, 0x03, // LDA $03,S
            0xA8, // TAY
            0xB9, 0x01, 0x00, // LDA $0001,Y
            0x29, 0xFF, 0x00, // AND #$00FF
            0xAA, // TAX
            0x98, // TYA
            0x18, // CLC
            0x69, 0x08, 0x00, // ADC #$0008
            0x83, 0x03, // STA $03,S
            0xAB, // PLB
            0x28, // PLP
            0x6B, // RTL
        ]);
        assert_eq!(
            detect_inline_arg_bytes(&rom, RomMapping::LoRom, 0, 0x8000, 1, 1),
            Some(8)
        );
    }

    #[test]
    fn rejects_y_carrier_after_y_mutation() {
        let rom = rom_at_8000(&[
            0xC2, 0x30, // REP #$30
            0xA3, 0x03, // LDA $03,S
            0xA8, // TAY
            0xC8, // INY
            0x98, // TYA
            0x18, // CLC
            0x69, 0x08, 0x00, // ADC #$0008
            0x83, 0x03, // STA $03,S
            0x6B, // RTL
        ]);
        assert_eq!(
            detect_inline_arg_bytes(&rom, RomMapping::LoRom, 0, 0x8000, 1, 1),
            None
        );
    }

    fn k(pc: u32) -> DecodeKey {
        DecodeKey::new(addr24(0, pc), 1, 1)
    }

    #[test]
    fn linear_then_rts() {
        // LDA #$01 (m=1, 2 bytes) ; RTS
        let rom = rom_at_8000(&[0xA9, 0x01, 0x60]);
        let g = decode_function(&rom, 0, 0x8000, 1, 1, None, &DecodeEnv::default());
        assert_eq!(g.len(), 2);
        let lda = g.get(&k(0x8000)).unwrap();
        assert_eq!(lda.insn.mnem, "LDA");
        assert_eq!(lda.successors, vec![k(0x8002)]);
        let rts = g.get(&k(0x8002)).unwrap();
        assert_eq!(rts.insn.mnem, "RTS");
        assert!(rts.successors.is_empty());
    }

    #[test]
    fn ptrcall_uses_pea_return_without_inlining_explicit_target() {
        // PEA $8008; JMP ($0010). The dispatched RTS resumes at $8009,
        // while $8010 is an independently discovered handler target.
        let mut bytes = vec![0u8; 0x12];
        bytes[0..6].copy_from_slice(&[0xF4, 0x08, 0x80, 0x6C, 0x10, 0x00]);
        bytes[9] = 0x60;
        bytes[0x10] = 0x60;
        let rom = rom_at_8000(&bytes);
        let dispatch = HashMap::from([(
            0x008003,
            IndirectDispatchSite {
                count: 1,
                idx_reg: 'X',
                table_bases: vec![],
                ptr_call: true,
                pointer_match: true,
                popped_call_frame: false,
                rts_stack: false,
                targets: vec![0x8010],
            },
        )]);
        let env = DecodeEnv {
            indirect_dispatch: Some(&dispatch),
            ..DecodeEnv::default()
        };
        let graph = decode_function(&rom, 0, 0x8000, 1, 1, None, &env);
        let jump = graph.get(&k(0x8003)).unwrap();
        assert_eq!(jump.insn.dispatch_entries, Some(vec![0x008010]));
        assert!(jump.insn.dispatch_call);
        assert!(jump.insn.dispatch_pointer_match);
        assert_eq!(jump.successors, vec![k(0x8009)]);
        assert!(graph.get(&k(0x8010)).is_none());
    }

    #[test]
    fn indirect_jsr_decode_cache_depends_on_each_handler_exit() {
        let mut bytes = vec![0u8; 0x12];
        bytes[0..3].copy_from_slice(&[0xFC, 0x10, 0x80]);
        bytes[3] = 0x60;
        bytes[0x10] = 0x60;
        let rom = rom_at_8000(&bytes);
        let dispatch = HashMap::from([(
            0x008000,
            IndirectDispatchSite {
                count: 1,
                idx_reg: 'X',
                table_bases: vec![],
                ptr_call: true,
                pointer_match: true,
                popped_call_frame: false,
                rts_stack: false,
                targets: vec![0x8010],
            },
        )]);
        let env = DecodeEnv {
            indirect_dispatch: Some(&dispatch),
            stop_on_unknown_callee_exit: true,
            ..DecodeEnv::default()
        };
        let graph = decode_function(&rom, 0, 0x8000, 1, 1, None, &env);
        let (exact, sets, _) = compute_deps(&graph, &env);
        assert!(exact.iter().any(|(key, _)| *key == (0x008010, 1, 1)));
        assert!(sets.iter().any(|(key, _)| *key == (0x008010, 1, 1)));
    }

    #[test]
    fn explicit_null_dispatch_target_stays_zero() {
        let rom = rom_at_8000(&[0xFC, 0x10, 0x80]);
        let insn = decode_insn(&rom, 0, 0x8000, 0, 1, 1).unwrap();
        assert_eq!(
            resolve_indirect_dispatch_targets(&rom, RomMapping::LoRom, 0, &insn, 1, &[], &[0], &[],),
            Some(vec![0])
        );
    }

    #[test]
    fn computed_pei_rts_is_not_a_callable_exit_mode() {
        let rom = rom_at_8000(&[
            0x8B, // PHB
            0x5A, // PHY (two bytes with X=0)
            0xE2, 0x20, // SEP #$20
            0xD4, 0x10, // PEI ($10)
            0x60, // computed RTS
        ]);
        let graph = decode_function(&rom, 0, 0x8000, 0, 0, None, &DecodeEnv::default());
        assert_eq!(analyze_function_exit_mx(&graph, None), (None, None));
        let (local, dependencies) = function_exit_mx_equation(&graph);
        assert_eq!(local, HashSet::from([(1, 0)]));
        assert!(dependencies.contains(&(0xFFFFFFFF, 0, 0)));
    }

    #[test]
    fn tsc_scratch_restore_proves_balanced_exit() {
        let rom = rom_at_8000(&[
            0x3B, // TSC
            0x85, 0x10, // STA $10
            0xA9, 0x34, 0x12, // LDA #$1234
            0x1B, // TCS (temporary)
            0xA5, 0x10, // LDA $10
            0x1B, // TCS (restore)
            0x60, // RTS
        ]);
        let graph = decode_function(&rom, 0, 0x8000, 0, 0, None, &DecodeEnv::default());
        assert_eq!(analyze_function_exit_mx(&graph, None), (Some(0), Some(0)));
    }

    #[test]
    fn clobbered_tsc_scratch_keeps_exit_unproven() {
        let rom = rom_at_8000(&[
            0x3B, // TSC
            0x85, 0x10, // STA $10
            0x64, 0x10, // STZ $10
            0xA5, 0x10, // LDA $10
            0x1B, // TCS
            0x60, // RTS
        ]);
        let graph = decode_function(&rom, 0, 0x8000, 0, 0, None, &DecodeEnv::default());
        assert_eq!(analyze_function_exit_mx(&graph, None), (None, None));
    }

    #[test]
    fn partial_frame_return_is_not_a_callable_exit() {
        let rom = rom_at_8000(&[
            0xD0, 0x01, // BNE $8003
            0x60, // balanced RTS
            0x8B, // PHB
            0x60, // consumes local byte and caller frame byte
        ]);
        let graph = decode_function(&rom, 0, 0x8000, 0, 0, None, &DecodeEnv::default());
        assert_eq!(analyze_function_exit_mx(&graph, None), (Some(0), Some(0)));
        let (local, dependencies) = function_exit_mx_equation(&graph);
        assert_eq!(local, HashSet::from([(0, 0)]));
        assert!(!dependencies.contains(&(0xFFFFFFFF, 0, 0)));
    }

    #[test]
    fn runtime_pointer_jsr_preserves_fallthrough() {
        // JSR ($0FA8,X) reads a runtime pointer from low RAM and returns to
        // the following RTS. It must not be treated as a phantom ROM table.
        let rom = rom_at_8000(&[0xFC, 0xA8, 0x0F, 0x60]);
        let graph = decode_function(&rom, 0, 0x8000, 1, 1, None, &DecodeEnv::default());
        let call = graph.get(&k(0x8000)).unwrap();
        assert!(call.insn.dispatch_runtime);
        assert_eq!(call.successors, vec![k(0x8003)]);
        assert!(graph.suppressed_indirect_calls.is_empty());
    }

    #[test]
    fn empty_callee_exit_set_makes_direct_call_terminal() {
        let rom = rom_at_8000(&[0x20, 0x00, 0x90, 0x00]);
        let exit_sets = HashMap::from([((0x009000, 1, 1), Vec::new())]);
        let env = DecodeEnv {
            callee_exit_mx_modes: Some(&exit_sets),
            stop_on_unknown_callee_exit: true,
            ..DecodeEnv::default()
        };
        let graph = decode_function(&rom, 0, 0x8000, 1, 1, None, &env);
        assert_eq!(graph.len(), 1);
        assert!(graph.unknown_callee_exit_sites.is_empty());
        assert!(graph.get(&k(0x8000)).unwrap().successors.is_empty());
    }

    #[test]
    fn declared_terminal_jsr_does_not_decode_fallthrough() {
        let rom = rom_at_8000(&[0x20, 0x00, 0x90, 0x00]);
        let sites = std::collections::BTreeSet::from([0x008000]);
        let env = DecodeEnv {
            terminal_jsr_sites: Some(&sites),
            stop_on_unknown_callee_exit: true,
            ..DecodeEnv::default()
        };
        let graph = decode_function(&rom, 0, 0x8000, 1, 1, None, &env);
        assert_eq!(graph.len(), 1);
        assert!(graph.unknown_callee_exit_sites.is_empty());
        let call = graph.get(&k(0x8000)).unwrap();
        assert!(call.insn.terminal_jsr);
        assert!(call.successors.is_empty());
    }

    #[test]
    fn recovers_local_stride_runway() {
        let mut rom = rom_at_8000(&[]);
        let pattern = [
            0x4A, 0x85, 0x12, 0x4A, 0x65, 0x12, 0x18, 0x69, 0x00, 0x90, 0x85, 0x12, 0xA9, 0x00,
            0x00, 0xE2, 0x30, 0x6C, 0x12, 0x00,
        ];
        rom[..pattern.len()].copy_from_slice(&pattern);
        for index in 0..4usize {
            let offset = 0x1000 + index * 3;
            let store = 0x2000 + index as u16 * 4;
            rom[offset..offset + 3].copy_from_slice(&[0x8D, store as u8, (store >> 8) as u8]);
        }
        let jump = decode_insn(&rom, 17, 0x8011, 0, 1, 1).unwrap();
        assert_eq!(
            autorecover_local_stride_runway(
                &rom,
                RomMapping::LoRom,
                0,
                0x8000,
                0x8011,
                &jump,
                None,
                None,
                &[],
            ),
            Some(vec![0x009000, 0x009003, 0x009006, 0x009009])
        );
    }

    #[test]
    fn cond_branch_two_successors() {
        // BEQ ->$8005 (fall $8002) ; RTS@$8002 ; pad ; RTS@$8005
        let mut bytes = vec![0u8; 6];
        bytes[0] = 0xF0; // BEQ
        bytes[1] = 0x03; // target = $8002 + 3 = $8005
        bytes[2] = 0x60; // RTS @ $8002
        bytes[5] = 0x60; // RTS @ $8005
        let rom = rom_at_8000(&bytes);
        let g = decode_function(&rom, 0, 0x8000, 1, 1, None, &DecodeEnv::default());
        let beq = g.get(&k(0x8000)).unwrap();
        assert_eq!(beq.insn.mnem, "BEQ");
        // [fall, jump] order preserved.
        assert_eq!(beq.successors, vec![k(0x8002), k(0x8005)]);
        assert!(g.get(&k(0x8002)).unwrap().successors.is_empty());
        assert!(g.get(&k(0x8005)).unwrap().successors.is_empty());
        assert_eq!(g.len(), 3);
    }

    #[test]
    fn const_z_fold_prunes_dead_path() {
        // LDA #$00 (Z=1) ; BEQ ->$8006 ; RTS@$8004 (dead) ; RTS@$8006 (live)
        let mut bytes = vec![0u8; 7];
        bytes[0] = 0xA9; // LDA #imm (m=1)
        bytes[1] = 0x00; // imm 0 -> Z=1
        bytes[2] = 0xF0; // BEQ @ $8002
        bytes[3] = 0x02; // target = $8004 + 2 = $8006
        bytes[4] = 0x60; // RTS @ $8004 (fall, dead once folded)
        bytes[6] = 0x60; // RTS @ $8006 (jump, live)
        let rom = rom_at_8000(&bytes);
        let g = decode_function(&rom, 0, 0x8000, 1, 1, None, &DecodeEnv::default());
        let beq = g.get(&k(0x8002)).unwrap();
        // BEQ folded to single live edge ($8006).
        assert_eq!(beq.successors, vec![k(0x8006)]);
        assert!(beq.insn.const_z_fold_unconditional);
        // Dead fall-through ($8004) pruned by reachability.
        assert!(g.get(&k(0x8004)).is_none());
        assert!(g.get(&k(0x8006)).is_some());
        assert_eq!(g.const_z_folds.len(), 1);
    }

    #[test]
    fn jml_is_always_a_separate_tail_demand() {
        // Even a same-bank JML crosses the current routine boundary.
        let mut bytes = vec![0u8; 6];
        bytes[0] = 0x5C; // JMP long (JML)
        bytes[1] = 0x05;
        bytes[2] = 0x80;
        bytes[3] = 0x00; // bank 00
        bytes[5] = 0x60; // RTS @ $8005
        let rom = rom_at_8000(&bytes);
        let g = decode_function(&rom, 0, 0x8000, 1, 1, None, &DecodeEnv::default());
        let jml = g.get(&k(0x8000)).unwrap();
        assert!(jml.successors.is_empty());
        assert_eq!(g.len(), 1);

        // Cross-bank JML -> no static successor.
        let mut b2 = vec![0u8; 4];
        b2[0] = 0x5C;
        b2[1] = 0x00;
        b2[2] = 0x80;
        b2[3] = 0x07; // bank 07 != 00
        let rom2 = rom_at_8000(&b2);
        let g2 = decode_function(&rom2, 0, 0x8000, 1, 1, None, &DecodeEnv::default());
        assert!(g2.get(&k(0x8000)).unwrap().successors.is_empty());
        assert_eq!(g2.len(), 1);
    }

    #[test]
    fn indirect_long_jmp_uses_long_dispatch_width() {
        let rom = rom_at_8000(&[0xDC, 0x34, 0x12]);
        let insn = decode_insn(&rom, 0, 0x8000, 0, 1, 1).unwrap();
        assert_eq!(insn.length, 3);
        assert!(is_long_dispatch(&insn, &[]));
    }

    #[test]
    fn php_plp_brackets_mx() {
        // PHP pushes (m,x); SEP changes them; PLP restores.
        let php = decode_insn(&[0x08], 0, 0x8000, 0, 1, 1).unwrap();
        let sep = decode_insn(&[0xE2, 0x30], 0, 0x8001, 0, 1, 1).unwrap();
        let plp = decode_insn(&[0x28], 0, 0x8003, 0, 1, 1).unwrap();
        let (m, x, s1) = post_state(&php, 0, 1, &[]);
        assert_eq!((m, x), (0, 1));
        assert_eq!(s1, vec![(0, 1)]);
        let (m, x, s2) = post_state(&sep, m, x, &s1);
        assert_eq!((m, x), (1, 1));
        assert_eq!(s2, vec![(0, 1)]); // SEP doesn't touch p_stack
        let (m, x, s3) = post_state(&plp, m, x, &s2);
        assert_eq!((m, x), (0, 1)); // restored
        assert!(s3.is_empty());
    }
}
