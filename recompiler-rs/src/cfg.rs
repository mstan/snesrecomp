//! Parse a v1-format bank cfg file into a `BankCfg` (port of
//! `recompiler/v2/cfg_loader.py`).
//!
//! v2 ignores the v1 ABI-fiction directives (`sig:`, `ret_y`, `y_after:`, …);
//! every v2 function is `void f(CpuState *cpu)`. Unrecognized directives are
//! silently ignored (forward-compat); a handful of malformed directives are
//! hard errors, matching the Python `raise ValueError`.
//!
//! Ordering of map/set-backed directives uses `BTreeMap`/`BTreeSet` so output
//! is deterministic regardless of input order (the Python relied on dict
//! insertion order / nondeterministic set order).

use std::collections::{BTreeMap, BTreeSet};
use std::path::Path;

use crate::rom::RelocRegion;

/// A `func` emit entry. Mirrors the Python `BankEntry` (defined in emit_bank.py).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BankEntry {
    pub name: Option<String>,
    pub start: u32,       // 16-bit local PC
    pub end: Option<u32>, // exclusive end PC (None = run to terminator)
    pub entry_m: u8,      // entry M flag (0 or 1), default 1
    pub entry_x: u8,      // entry X flag (0 or 1), default 1
    pub tail_call_pc16: Option<u32>,
    pub entry_s_offset: i32, // stack adjustment at entry (tail-call imbalance)
    pub force_variants: Option<Vec<(u8, u8)>>, // extra (m,x) pairs to force-generate
    pub exit_mx: Option<(u8, u8)>, // callee-exit (m,x) override
    pub inline_skip: Option<i32>, // JSR-inline-param skip bytes
    /// cfg `force_host_return:<hex>[,<hex>]` — RTS/RTL SITE addresses (parsed
    /// values, masked 24-bit; emit adds the bank) whose terminal host-returns NORMAL.
    pub force_host_return_sites: BTreeSet<u32>,
}

impl BankEntry {
    /// Construct with the Python defaults (entry_m=entry_x=1, rest empty).
    pub fn new(name: Option<String>, start: u32) -> Self {
        BankEntry {
            name,
            start,
            end: None,
            entry_m: 1,
            entry_x: 1,
            tail_call_pc16: None,
            entry_s_offset: 0,
            force_variants: None,
            exit_mx: None,
            inline_skip: None,
            force_host_return_sites: BTreeSet::new(),
        }
    }
}

/// A `name <addr> <friendly>` line — cross-bank label / friendly name.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct NameDecl {
    pub addr_24: u32, // bank << 16 | local pc
    pub name: String,
}

/// An `indirect_dispatch` directive (authorises static recovery of an indirect
/// JMP/JML/JSR's target list).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct IndirectDispatch {
    pub site_pc16: u32,
    pub count: u32,
    pub idx_reg: char,         // 'X' or 'Y'
    pub table_bases: Vec<u32>, // 0..3 entries (see cfg_loader doc)
    /// Pointer-sourced call (`PEA <ret>; JMP (ptr)` / `JSR (ptr)`).
    pub ptr_call: bool,
    /// Match an explicit runtime pointer value rather than indexing a ROM
    /// table. True for ptrcall/ptrtail/ptrtail_popcall.
    pub pointer_match: bool,
    /// The dispatcher consumed the two-byte JSR frame before tail transfer.
    pub popped_call_frame: bool,
    /// PEI/PHA + RTS internal computed transfer, not a whole-program call.
    pub rts_stack: bool,
    /// Explicit 16- or 24-bit targets for a pointer-sourced call.
    pub targets: Vec<u32>,
}

/// Parsed bank cfg. Field names mirror the Python `BankCfg` dataclass.
#[derive(Debug, Clone, Default)]
pub struct BankCfg {
    pub bank: i32, // -1 until a `bank = NN` line is seen
    pub includes: Vec<String>,
    pub entries: Vec<BankEntry>,
    pub names: Vec<NameDecl>,
    pub exclude_ranges: Vec<(u32, u32)>,
    pub data_regions: Vec<(u32, u32, u32)>, // (bank, start, end)
    pub reloc_regions: Vec<RelocRegion>,
    pub exit_mx_at: Vec<(u8, u32, u8, u8)>, // (bank, addr16, m, x)
    pub exit_mx_at_per_variant: Vec<(u8, u32, u8, u8, u8, u8)>,
    pub auto_vectors: bool,
    pub indirect_dispatch: Vec<IndirectDispatch>,
    pub inline_dispatch_loops: BTreeSet<u32>,
    pub terminal_jsr: BTreeSet<u32>,
    pub hle_spc_upload: Vec<u32>,
    pub hle_func: BTreeMap<u32, String>, // pc16 -> c_function_name
    pub hle_dispatch: BTreeMap<u32, String>, // site_pc16 -> c_function_name
    pub force_variant_at: BTreeMap<u32, (u8, u8)>, // site_pc24 -> (m, x)
}

impl BankCfg {
    fn new() -> Self {
        BankCfg {
            bank: -1,
            ..Default::default()
        }
    }
}

fn parse_hex(token: &str) -> Result<u32, String> {
    let t = token
        .strip_prefix("0x")
        .or_else(|| token.strip_prefix("0X"))
        .unwrap_or(token);
    u32::from_str_radix(t, 16).map_err(|_| format!("bad hex {token:?}"))
}

/// Strip a trailing `# ...` comment.
fn strip_comment(line: &str) -> &str {
    match line.find('#') {
        Some(idx) => line[..idx].trim_end(),
        None => line.trim_end(),
    }
}

fn is_valid_c_ident(name: &str) -> bool {
    // c_name.replace('_', '').isalnum() and not c_name[0].isdigit()
    let stripped: String = name.chars().filter(|&c| c != '_').collect();
    !stripped.is_empty()
        && stripped.chars().all(|c| c.is_ascii_alphanumeric())
        && !name
            .chars()
            .next()
            .map(|c| c.is_ascii_digit())
            .unwrap_or(true)
}

/// Parse a v1-format bank cfg file. `Err` on a malformed `bank =` / strict
/// directive; unrecognized directives are silently ignored.
pub fn load_bank_cfg<P: AsRef<Path>>(path: P) -> Result<BankCfg, String> {
    let path_disp = path.as_ref().display().to_string();
    // Python opens with errors='replace' — tolerate invalid UTF-8 (cfgs are
    // ASCII in practice, but a stray byte must not abort the load).
    let bytes = std::fs::read(&path).map_err(|e| format!("{path_disp}: {e}"))?;
    let text = String::from_utf8_lossy(&bytes);
    parse_bank_cfg(&text, &path_disp)
}

/// Core parser, factored out so tests can feed cfg text directly.
pub fn parse_bank_cfg(text: &str, path: &str) -> Result<BankCfg, String> {
    let mut cfg = BankCfg::new();
    let mut entry_mx_at: BTreeMap<u32, (u8, u8)> = BTreeMap::new();
    let mut end_at: BTreeMap<u32, u32> = BTreeMap::new();

    for raw in text.lines() {
        let stripped = strip_comment(raw).trim();
        if stripped.is_empty() {
            continue;
        }
        let tokens: Vec<&str> = stripped.split_whitespace().collect();
        let head = tokens[0];

        // bank = NN
        if head == "bank" && tokens.len() >= 3 && tokens[1] == "=" {
            cfg.bank = parse_hex(tokens[2])? as i32;
            continue;
        }
        // includes = a.h b.h c.h
        if head == "includes" && tokens.len() >= 3 && tokens[1] == "=" {
            cfg.includes = tokens[2..].iter().map(|s| s.to_string()).collect();
            continue;
        }
        // comment = ... (ignored)
        if head == "comment" && stripped.contains('=') {
            continue;
        }
        if head == "auto_vectors" {
            cfg.auto_vectors = true;
            continue;
        }
        if head == "entry_mx_at" {
            if tokens.len() != 4 {
                return Err(format!(
                    "{path}: entry_mx_at needs <pc16> <m> <x>, got: {stripped:?}"
                ));
            }
            let pc16 = parse_hex(tokens[1])
                .map_err(|e| format!("{path}: entry_mx_at bad argument: {e}"))?
                & 0xFFFF;
            let m = tokens[2]
                .parse::<i32>()
                .map_err(|e| format!("{path}: entry_mx_at bad argument: {e}"))?;
            let x = tokens[3]
                .parse::<i32>()
                .map_err(|e| format!("{path}: entry_mx_at bad argument: {e}"))?;
            entry_mx_at.insert(pc16, ((m & 1) as u8, (x & 1) as u8));
            continue;
        }
        if head == "end_at" {
            if tokens.len() != 3 {
                return Err(format!(
                    "{path}: end_at needs <pc16> <end>, got: {stripped:?}"
                ));
            }
            let pc16 = parse_hex(tokens[1])
                .map_err(|e| format!("{path}: end_at bad argument: {e}"))?
                & 0xFFFF;
            let end =
                parse_hex(tokens[2]).map_err(|e| format!("{path}: end_at bad argument: {e}"))?;
            end_at.insert(pc16, end);
            continue;
        }
        // hle_spc_upload <hex_pc> [legacy|live]
        // The Rust analyzer only needs the entry address; the Python emitter
        // retains the optional protocol mode when selecting the runtime helper.
        if head == "hle_spc_upload" {
            if tokens.len() < 2 || tokens.len() > 3 {
                return Err(format!(
                    "{path}: hle_spc_upload needs <pc> and optional [legacy|live], got: {stripped:?}"
                ));
            }
            if tokens.len() == 3 && tokens[2] != "legacy" && tokens[2] != "live" {
                return Err(format!(
                    "{path}: hle_spc_upload mode must be legacy or live, got: {:?}",
                    tokens[2]
                ));
            }
            let pc16 =
                parse_hex(tokens[1]).map_err(|e| format!("{path}: hle_spc_upload {e}"))? & 0xFFFF;
            cfg.hle_spc_upload.push(pc16);
            continue;
        }
        // hle_func <pc16> <c_function_name>
        if head == "hle_func" {
            if tokens.len() != 3 {
                return Err(format!(
                    "{path}: hle_func needs <pc> <c_function_name>, got: {stripped:?}"
                ));
            }
            let pc16 = parse_hex(tokens[1]).map_err(|e| format!("{path}: hle_func {e}"))? & 0xFFFF;
            let c_name = tokens[2];
            if !is_valid_c_ident(c_name) {
                return Err(format!(
                    "{path}: hle_func c_function_name must be a valid C identifier, got: {c_name:?}"
                ));
            }
            cfg.hle_func.insert(pc16, c_name.to_string());
            continue;
        }
        // force_variant_at <site_pc24> <m> <x>
        if head == "force_variant_at" {
            if tokens.len() != 4 {
                return Err(format!(
                    "{path}: force_variant_at needs <site_pc24> <m> <x>, got: {stripped:?}"
                ));
            }
            let site_pc24 = parse_hex(tokens[1])
                .map_err(|e| format!("{path}: force_variant_at {e}"))?
                & 0xFFFFFF;
            let m_val: i32 = tokens[2]
                .parse()
                .map_err(|_| format!("{path}: force_variant_at m and x must be 0 or 1"))?;
            let x_val: i32 = tokens[3]
                .parse()
                .map_err(|_| format!("{path}: force_variant_at m and x must be 0 or 1"))?;
            if !(m_val == 0 || m_val == 1) || !(x_val == 0 || x_val == 1) {
                return Err(format!(
                    "{path}: force_variant_at m and x must be 0 or 1, got m={m_val} x={x_val}"
                ));
            }
            if cfg.force_variant_at.contains_key(&site_pc24) {
                return Err(format!(
                    "{path}: force_variant_at duplicate site ${site_pc24:06X}"
                ));
            }
            cfg.force_variant_at
                .insert(site_pc24, (m_val as u8, x_val as u8));
            continue;
        }
        // hle_dispatch <site_pc16> <c_function_name>
        if head == "hle_dispatch" {
            if tokens.len() != 3 {
                return Err(format!(
                    "{path}: hle_dispatch needs <site_pc16> <c_function_name>, got: {stripped:?}"
                ));
            }
            let pc16 =
                parse_hex(tokens[1]).map_err(|e| format!("{path}: hle_dispatch {e}"))? & 0xFFFF;
            let c_name = tokens[2];
            if !is_valid_c_ident(c_name) {
                return Err(format!(
                    "{path}: hle_dispatch c_function_name must be a valid C identifier, got: {c_name:?}"
                ));
            }
            cfg.hle_dispatch.insert(pc16, c_name.to_string());
            continue;
        }
        // indirect_dispatch <site_pc> <count> idx:<X|Y> [tables:<lo>[,<hi>[,<bank>]]]
        if head == "indirect_dispatch" {
            if tokens.len() < 4 {
                return Err(format!(
                    "{path}: indirect_dispatch needs at least <site_pc> <count> idx:<reg> — got: {stripped:?}"
                ));
            }
            let site_pc16 = parse_hex(tokens[1])
                .map_err(|e| format!("{path}: indirect_dispatch {e}"))?
                & 0xFFFF;
            // Python int(tokens[2], 0): accepts 0x.. or decimal.
            let count: i64 = parse_int_auto(tokens[2])
                .map_err(|_| format!("{path}: indirect_dispatch bad count {:?}", tokens[2]))?;
            if count <= 0 || count > 4096 {
                return Err(format!(
                    "{path}: indirect_dispatch count {count} out of range (1..4096)"
                ));
            }
            let pointer_modes: Vec<&str> = tokens[3..]
                .iter()
                .copied()
                .filter(|token| {
                    matches!(
                        *token,
                        "ptrcall" | "ptrtail" | "ptrtail_popcall" | "rtsstack"
                    )
                })
                .collect();
            if !pointer_modes.is_empty() {
                if pointer_modes.len() != 1 {
                    return Err(format!(
                        "{path}: indirect_dispatch chooses exactly one pointer mode: {stripped:?}"
                    ));
                }
                let pointer_mode = pointer_modes[0];
                let mut targets = Vec::new();
                for t in &tokens[3..] {
                    if *t == pointer_mode {
                        continue;
                    }
                    if let Some(raw) = t.strip_prefix("targets:") {
                        for target in raw.split(',').filter(|target| !target.is_empty()) {
                            targets.push(
                                parse_hex(target).map_err(|e| {
                                    format!("{path}: indirect_dispatch targets: {e}")
                                })? & 0xFFFFFF,
                            );
                        }
                    } else {
                        return Err(format!(
                            "{path}: indirect_dispatch {pointer_mode} unknown option {t:?}"
                        ));
                    }
                }
                if targets.is_empty() {
                    return Err(format!(
                        "{path}: indirect_dispatch ptrcall needs targets:<...> — got: {stripped:?}"
                    ));
                }
                if targets.len() != count as usize {
                    return Err(format!(
                        "{path}: indirect_dispatch {pointer_mode} count {count} != {} targets",
                        targets.len()
                    ));
                }
                cfg.indirect_dispatch.push(IndirectDispatch {
                    site_pc16,
                    count: count as u32,
                    idx_reg: 'X',
                    table_bases: Vec::new(),
                    ptr_call: pointer_mode == "ptrcall",
                    pointer_match: pointer_mode != "rtsstack",
                    popped_call_frame: pointer_mode == "ptrtail_popcall",
                    rts_stack: pointer_mode == "rtsstack",
                    targets,
                });
                continue;
            }

            let mut idx_reg: Option<char> = None;
            let mut table_bases: Vec<u32> = Vec::new();
            for t in &tokens[3..] {
                if let Some(v) = t.strip_prefix("idx:") {
                    let v = v.to_ascii_uppercase();
                    if v != "X" && v != "Y" {
                        return Err(format!(
                            "{path}: indirect_dispatch idx: must be X or Y, got {v:?}"
                        ));
                    }
                    idx_reg = Some(v.chars().next().unwrap());
                } else if let Some(v) = t.strip_prefix("tables:") {
                    let raw_bases: Vec<&str> = v.split(',').collect();
                    if raw_bases.is_empty() || raw_bases.len() > 3 {
                        return Err(format!(
                            "{path}: indirect_dispatch tables: needs 1-3 comma-separated hex addresses, got {t:?}"
                        ));
                    }
                    let mut bases = Vec::with_capacity(raw_bases.len());
                    for b in raw_bases {
                        bases.push(
                            parse_hex(b)
                                .map_err(|e| format!("{path}: indirect_dispatch tables: {e}"))?
                                & 0xFFFF,
                        );
                    }
                    table_bases = bases;
                } else {
                    return Err(format!("{path}: indirect_dispatch unknown option {t:?}"));
                }
            }
            let idx_reg = idx_reg.ok_or_else(|| {
                format!("{path}: indirect_dispatch needs idx:X or idx:Y — got: {stripped:?}")
            })?;
            cfg.indirect_dispatch.push(IndirectDispatch {
                site_pc16,
                count: count as u32,
                idx_reg,
                table_bases,
                ptr_call: false,
                pointer_match: false,
                popped_call_frame: false,
                rts_stack: false,
                targets: Vec::new(),
            });
            continue;
        }
        // inline_dispatch_loop <site_pc16>
        if head == "inline_dispatch_loop" {
            if tokens.len() < 2 {
                return Err(format!(
                    "{path}: inline_dispatch_loop needs <site_pc16> — got: {stripped:?}"
                ));
            }
            let site_pc16 = parse_hex(tokens[1])
                .map_err(|e| format!("{path}: inline_dispatch_loop {e}"))?
                & 0xFFFF;
            cfg.inline_dispatch_loops.insert(site_pc16);
            continue;
        }
        // terminal_jsr <site_pc16>
        if head == "terminal_jsr" {
            if tokens.len() != 2 {
                return Err(format!(
                    "{path}: terminal_jsr needs exactly one <site_pc16>, got: {stripped:?}"
                ));
            }
            let site_pc16 =
                parse_hex(tokens[1]).map_err(|e| format!("{path}: terminal_jsr {e}"))? & 0xFFFF;
            if !cfg.terminal_jsr.insert(site_pc16) {
                return Err(format!(
                    "{path}: terminal_jsr duplicate site ${site_pc16:04X}"
                ));
            }
            continue;
        }
        // func <name> <hex_pc> [end:..] [tail_call:..] [exit_mx:M,X] ...
        if head == "func" {
            if tokens.len() < 3 {
                continue;
            }
            let name = tokens[1].to_string();
            // Python `_parse_hex(tokens[2])` raises on bad hex (no try/except in
            // the func branch), aborting the whole parse — propagate, don't skip.
            let start = parse_hex(tokens[2]).map_err(|e| format!("{path}: func {e}"))?;
            let mut end: Option<u32> = None;
            let mut tail_call_pc16: Option<u32> = None;
            let mut exit_mx: Option<(u8, u8)> = None;
            let mut entry_mx: Option<(u8, u8)> = None;
            let mut entry_s_offset_val: i32 = 0;
            let mut inline_skip_val: Option<i32> = None;
            let mut force_variants_val: Option<Vec<(u8, u8)>> = None;
            let mut force_host_return_sites_val: BTreeSet<u32> = BTreeSet::new();
            for t in &tokens[3..] {
                if let Some(v) = t.strip_prefix("end:") {
                    if let Ok(e) = parse_hex(v) {
                        end = Some(e);
                    }
                } else if let Some(v) = t.strip_prefix("tail_call:") {
                    if let Ok(e) = parse_hex(v) {
                        tail_call_pc16 = Some(e);
                    }
                } else if let Some(v) = t.strip_prefix("exit_mx:") {
                    if let Some(mx) = parse_mx_pair(v) {
                        exit_mx = Some(mx);
                    }
                } else if let Some(v) = t.strip_prefix("entry_mx:") {
                    if let Some(mx) = parse_mx_pair(v) {
                        entry_mx = Some(mx);
                    }
                } else if let Some(v) = t.strip_prefix("force_variants:") {
                    // Python wraps the whole loop in one try: a 2-element pair
                    // whose int() fails discards the ENTIRE option (not just
                    // that pair). A non-2-element split is skipped silently.
                    let mut pairs = Vec::new();
                    let mut aborted = false;
                    for pair in v.split(';') {
                        let pv: Vec<&str> = pair.split(',').collect();
                        if pv.len() == 2 {
                            match (pv[0].parse::<i32>(), pv[1].parse::<i32>()) {
                                (Ok(m), Ok(x)) => pairs.push(((m & 1) as u8, (x & 1) as u8)),
                                _ => {
                                    aborted = true;
                                    break;
                                }
                            }
                        }
                    }
                    if !aborted && !pairs.is_empty() {
                        force_variants_val = Some(pairs);
                    }
                } else if let Some(v) = t.strip_prefix("entry_s_offset:") {
                    if let Ok(n) = v.parse::<i32>() {
                        entry_s_offset_val = n;
                    }
                } else if let Some(v) = t.strip_prefix("inline_skip:") {
                    if let Ok(n) = v.parse::<i32>() {
                        inline_skip_val = Some(n);
                    }
                } else if let Some(raw) = t.strip_prefix("force_host_return") {
                    // `force_host_return:<hex>[,<hex>]` — terminal RTS/RTL at the
                    // named SITE(s) host-returns RECOMP_RETURN_NORMAL, bypassing
                    // balanced-stack / ancestor-skip / dispatch. Lets a multi-
                    // return function host-return only its terminal. A bare token
                    // (no `:`) contributes no sites (matches the Python).
                    if let Some(list) = raw.strip_prefix(':') {
                        for tok in list.split(',') {
                            let tok = tok.trim();
                            if tok.is_empty() {
                                continue;
                            }
                            if let Ok(v) = u32::from_str_radix(tok, 16) {
                                force_host_return_sites_val.insert(v & 0xFFFFFF);
                            }
                        }
                    }
                }
            }
            let mut be = BankEntry::new(Some(name.clone()), start);
            be.end = end;
            be.tail_call_pc16 = tail_call_pc16;
            be.entry_s_offset = entry_s_offset_val;
            be.exit_mx = exit_mx;
            be.inline_skip = inline_skip_val;
            be.force_variants = force_variants_val;
            be.force_host_return_sites = force_host_return_sites_val;
            // Entry-mode seed: explicit entry_mx: wins; else *_STRAT/_ISTRAT → M1X0.
            if let Some((m, x)) = entry_mx {
                be.entry_m = m;
                be.entry_x = x;
            } else if name.ends_with("_STRAT") || name.ends_with("_ISTRAT") {
                be.entry_m = 1;
                be.entry_x = 0;
            }
            cfg.entries.push(be);
            continue;
        }
        // name <hex_addr> <friendly_name>
        if head == "name" {
            if tokens.len() < 3 {
                continue;
            }
            let addr = match parse_hex(tokens[1]) {
                Ok(v) => v,
                Err(_) => continue,
            };
            cfg.names.push(NameDecl {
                addr_24: addr,
                name: tokens[2].to_string(),
            });
            continue;
        }
        // exclude_range <start> <end>
        if head == "exclude_range" && tokens.len() >= 3 {
            if let (Ok(s), Ok(e)) = (parse_hex(tokens[1]), parse_hex(tokens[2])) {
                cfg.exclude_ranges.push((s, e));
            }
            continue;
        }
        // exit_mx_at <hex_24bit_addr> <m> <x>
        if head == "exit_mx_at" && tokens.len() >= 4 {
            if let (Ok(addr_24), Ok(m_val), Ok(x_val)) = (
                parse_hex(tokens[1]),
                tokens[2].parse::<i32>(),
                tokens[3].parse::<i32>(),
            ) {
                let bank_id = ((addr_24 >> 16) & 0xFF) as u8;
                let addr16 = addr_24 & 0xFFFF;
                cfg.exit_mx_at
                    .push((bank_id, addr16, (m_val & 1) as u8, (x_val & 1) as u8));
            }
            continue;
        }
        // reloc <ram_bank> <ram_addr> <rom_bank> <rom_off> <len>
        if head == "reloc" {
            if tokens.len() != 6 {
                return Err(format!(
                    "{path}: reloc needs <ram_bank> <ram_addr> <rom_bank> <rom_off> <len> (5 hex args), got: {stripped:?}"
                ));
            }
            let ram_bank = parse_hex(tokens[1])
                .map_err(|e| format!("{path}: reloc bad hex operand: {e}"))?
                & 0xFF;
            let ram_addr = parse_hex(tokens[2])
                .map_err(|e| format!("{path}: reloc bad hex operand: {e}"))?
                & 0xFFFF;
            let rom_bank = parse_hex(tokens[3])
                .map_err(|e| format!("{path}: reloc bad hex operand: {e}"))?
                & 0xFF;
            let rom_off = parse_hex(tokens[4])
                .map_err(|e| format!("{path}: reloc bad hex operand: {e}"))?
                & 0xFFFF;
            let length =
                parse_hex(tokens[5]).map_err(|e| format!("{path}: reloc bad hex operand: {e}"))?;
            if length == 0 {
                return Err(format!(
                    "{path}: reloc length must be positive, got ${length:X}"
                ));
            }
            cfg.reloc_regions.push(RelocRegion::new(
                ram_bank, ram_addr, rom_bank, rom_off, length,
            ));
            continue;
        }
        // data_region <bank> <start> <end>
        if head == "data_region" && tokens.len() >= 4 {
            if let (Ok(b), Ok(s), Ok(e)) = (
                parse_hex(tokens[1]),
                parse_hex(tokens[2]),
                parse_hex(tokens[3]),
            ) {
                cfg.data_regions.push((b, s, e));
            }
            continue;
        }
        // Anything else: silently ignore.
    }

    if cfg.bank < 0 {
        return Err(format!("{path}: missing 'bank = NN' line"));
    }

    for entry in &mut cfg.entries {
        if let Some(&(m, x)) = entry_mx_at.get(&(entry.start & 0xFFFF)) {
            entry.entry_m = m;
            entry.entry_x = x;
        }
        if let Some(&end) = end_at.get(&(entry.start & 0xFFFF)) {
            entry.end = Some(end);
        }
    }

    // Auto-promote in-bank `name <addr> <friendly>` decls to emit entries.
    let mut existing_starts: BTreeSet<u32> = cfg.entries.iter().map(|e| e.start & 0xFFFF).collect();
    let bank = cfg.bank;
    let promotions: Vec<NameDecl> = cfg.names.clone();
    for nd in promotions {
        if ((nd.addr_24 >> 16) & 0xFF) as i32 != bank {
            continue;
        }
        let local_pc = nd.addr_24 & 0xFFFF;
        if existing_starts.contains(&local_pc) {
            continue;
        }
        cfg.entries.push(BankEntry::new(Some(nd.name), local_pc));
        existing_starts.insert(local_pc);
    }

    Ok(cfg)
}

/// Parse "M,X" → (m&1, x&1), or None if malformed (matches the Python
/// try/except that silently leaves the field unset).
fn parse_mx_pair(s: &str) -> Option<(u8, u8)> {
    let parts: Vec<&str> = s.split(',').collect();
    if parts.len() != 2 {
        return None;
    }
    let m: i32 = parts[0].parse().ok()?;
    let x: i32 = parts[1].parse().ok()?;
    Some(((m & 1) as u8, (x & 1) as u8))
}

/// Python `int(tok, 0)`: decimal, or 0x/0o/0b prefixed.
fn parse_int_auto(tok: &str) -> Result<i64, std::num::ParseIntError> {
    if let Some(h) = tok.strip_prefix("0x").or_else(|| tok.strip_prefix("0X")) {
        i64::from_str_radix(h, 16)
    } else if let Some(o) = tok.strip_prefix("0o").or_else(|| tok.strip_prefix("0O")) {
        i64::from_str_radix(o, 8)
    } else if let Some(b) = tok.strip_prefix("0b").or_else(|| tok.strip_prefix("0B")) {
        i64::from_str_radix(b, 2)
    } else {
        tok.parse::<i64>()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn missing_bank_errors() {
        assert!(parse_bank_cfg("func Foo 8000\n", "t").is_err());
    }

    #[test]
    fn basic_func_and_bank() {
        let cfg = parse_bank_cfg("bank = 07\nfunc I_RESET d127\n", "t").unwrap();
        assert_eq!(cfg.bank, 0x07);
        assert_eq!(cfg.entries.len(), 1);
        let e = &cfg.entries[0];
        assert_eq!(e.name.as_deref(), Some("I_RESET"));
        assert_eq!(e.start, 0xD127);
        assert_eq!((e.entry_m, e.entry_x), (1, 1));
    }

    #[test]
    fn strat_entry_seed() {
        let cfg = parse_bank_cfg("bank = 00\nfunc M8_STRAT 8006\n", "t").unwrap();
        let e = &cfg.entries[0];
        assert_eq!((e.entry_m, e.entry_x), (1, 0));
    }

    #[test]
    fn func_options() {
        let cfg = parse_bank_cfg(
            "bank = 00\nfunc Foo 8000 end:8100 exit_mx:1,0 entry_mx:0,1 inline_skip:3 force_variants:1,1;0,0\n",
            "t",
        )
        .unwrap();
        let e = &cfg.entries[0];
        assert_eq!(e.end, Some(0x8100));
        assert_eq!(e.exit_mx, Some((1, 0)));
        assert_eq!((e.entry_m, e.entry_x), (0, 1)); // entry_mx wins
        assert_eq!(e.inline_skip, Some(3));
        assert_eq!(e.force_variants, Some(vec![(1, 1), (0, 0)]));
    }

    #[test]
    fn reloc_and_data_region() {
        let cfg = parse_bank_cfg(
            "bank = 02\nreloc 7E 321F 02 8000 5C00\ndata_region 03 e000 e100\n",
            "t",
        )
        .unwrap();
        assert_eq!(cfg.reloc_regions.len(), 1);
        assert_eq!(
            cfg.reloc_regions[0],
            RelocRegion::new(0x7E, 0x321F, 0x02, 0x8000, 0x5C00)
        );
        assert_eq!(cfg.data_regions, vec![(0x03, 0xE000, 0xE100)]);
    }

    #[test]
    fn indirect_dispatch_parse() {
        let cfg = parse_bank_cfg(
            "bank = 03\nindirect_dispatch e19e 8 idx:X tables:9000,9100\n",
            "t",
        )
        .unwrap();
        assert_eq!(cfg.indirect_dispatch.len(), 1);
        let d = &cfg.indirect_dispatch[0];
        assert_eq!(d.site_pc16, 0xE19E);
        assert_eq!(d.count, 8);
        assert_eq!(d.idx_reg, 'X');
        assert_eq!(d.table_bases, vec![0x9000, 0x9100]);
        assert!(!d.ptr_call);
        assert!(!d.pointer_match);
        assert!(!d.popped_call_frame);
        assert!(!d.rts_stack);
        assert!(d.targets.is_empty());
    }

    #[test]
    fn ptrcall_targets_preserve_16_and_24_bit_values() {
        let cfg = parse_bank_cfg(
            "bank = 08\nindirect_dispatch 852c 3 ptrcall targets:8569,8884B8,91D27F\n",
            "t",
        )
        .unwrap();
        let dispatch = &cfg.indirect_dispatch[0];
        assert!(dispatch.ptr_call);
        assert!(dispatch.pointer_match);
        assert_eq!(dispatch.idx_reg, 'X');
        assert_eq!(dispatch.targets, vec![0x8569, 0x8884B8, 0x91D27F]);
    }

    #[test]
    fn dkc2_pointer_tail_modes_parse() {
        let cfg = parse_bank_cfg(
            "bank = B3\n\
             indirect_dispatch A364 2 ptrtail targets:B39E10,B3A441\n\
             indirect_dispatch A010 1 ptrtail_popcall targets:B38052\n\
             indirect_dispatch F000 2 rtsstack targets:B3F100,B3F200\n",
            "t",
        )
        .unwrap();
        let tail = &cfg.indirect_dispatch[0];
        assert!(!tail.ptr_call && tail.pointer_match);
        assert!(!tail.popped_call_frame && !tail.rts_stack);
        let pop = &cfg.indirect_dispatch[1];
        assert!(!pop.ptr_call && pop.pointer_match && pop.popped_call_frame);
        assert!(!pop.rts_stack);
        let rts = &cfg.indirect_dispatch[2];
        assert!(!rts.ptr_call && !rts.pointer_match);
        assert!(!rts.popped_call_frame && rts.rts_stack);
    }

    #[test]
    fn terminal_jsr_parse() {
        let cfg = parse_bank_cfg("bank = B3\nterminal_jsr A436\n", "test.cfg").unwrap();
        assert_eq!(cfg.terminal_jsr, BTreeSet::from([0xA436]));
    }

    #[test]
    fn ptrcall_count_must_match_targets() {
        let err = parse_bank_cfg(
            "bank = 08\nindirect_dispatch 852c 2 ptrcall targets:8569\n",
            "t",
        )
        .unwrap_err();
        assert!(err.contains("count 2 != 1 targets"));
    }

    #[test]
    fn postdeclared_entry_and_end_overrides_apply() {
        let cfg = parse_bank_cfg(
            "bank = 02\nfunc Root 8000 end:8010\nentry_mx_at 8000 0 0\nend_at 8000 8020\n",
            "t",
        )
        .unwrap();
        let entry = &cfg.entries[0];
        assert_eq!((entry.entry_m, entry.entry_x), (0, 0));
        assert_eq!(entry.end, Some(0x8020));
    }

    #[test]
    fn name_autopromote_in_bank() {
        // In-bank name without a func entry → promoted to an entry.
        let cfg =
            parse_bank_cfg("bank = 01\nname 018640 Foo\nname 008000 CrossBank\n", "t").unwrap();
        // 018640 is in-bank (bank 01) → promoted; 008000 is bank 00 → not.
        assert!(cfg
            .entries
            .iter()
            .any(|e| e.name.as_deref() == Some("Foo") && e.start == 0x8640));
        assert!(!cfg
            .entries
            .iter()
            .any(|e| e.name.as_deref() == Some("CrossBank")));
        assert_eq!(cfg.names.len(), 2);
    }

    #[test]
    fn force_variant_dup_errors() {
        assert!(parse_bank_cfg(
            "bank = 00\nforce_variant_at 008100 1 0\nforce_variant_at 008100 0 1\n",
            "t"
        )
        .is_err());
    }
}
