"""snesrecomp.recompiler.v2.codegen

Emit C code from v2 IR. Generated functions take a single
`CpuState *cpu` parameter and mutate `cpu->A`, `cpu->X`, etc., directly
— no return values, no per-function locals masquerading as registers.

Replaces v1 EmitCtx C-expression-string-based codegen
(recomp.py:2829-6200) including the heuristic phi machinery
(_branch_states, _label_a/b/x/y, _emit_backedge_phi, _emit_branch,
_ensure_mutable_x). v2 codegen has no per-function abstract register
state at emit time — register reads/writes are explicit memory loads
and stores against the CpuState struct.

Every IR Value produced by an IR op becomes a fresh C local. A
`Value(vid=N)` lowers to `_v<N>`. Width is inferred per op (the IR
op type carries the width).

Public API:
    emit_block(block: IRBlock, *, indent: str = "  ") -> List[str]

Phase 5 of plan parsed-skipping-rainbow.md. Phase 6 will wire this
into a per-function emit driver (replacing the v1 emit_function) and
run the full SMW regen against it.
"""

import sys
import pathlib

_THIS_DIR = pathlib.Path(__file__).resolve().parent
_RECOMPILER_DIR = _THIS_DIR.parent
for p in (str(_THIS_DIR), str(_RECOMPILER_DIR)):
    if p not in sys.path:
        sys.path.insert(0, p)

from typing import Dict, List, Optional, Tuple  # noqa: E402

# Resolver: 24-bit address (bank << 16 | pc) -> friendly C function name.
# Populated by emit_bank before each bank emit (a process-wide map of every
# `func`/`name` declaration across all banks loaded so far). When a Call op
# resolves to one of these addresses, codegen emits the friendly name; else
# it falls back to the synthetic `bank_BB_AAAA` form.
_NAME_RESOLVER: Dict[int, str] = {}

# Set of (24-bit Call target, entry_m, entry_x) tuples for EVERY Call
# emitted, regardless of whether the friendly name resolved. v2_regen
# diffs this against the set of (addr, m, x) variants actually emitted
# and adds missing entries to cover any unmet demand.
#
# Why track ALL targets, not just unresolved ones: cfg-named targets
# (e.g. UpdateEntirePalette) might only have an M1X1 entry in cfg, but
# get called from M0X0 callers. The Call site needs UpdateEntirePalette
# _M0X0 to exist; tracking the (target, m, x) tuple lets v2_regen
# discover the unmet variant and clone the cfg entry at the new (m, x).
#
# Per-(m, x) tracking: a 65816 function decoded with M=1 X=1 is a
# different instruction stream than M=1 X=0 because LDX #imm consumes
# 2 vs 3 bytes (and LDA/LDY immediates similarly with M). So a single
# ROM function reachable from contexts with different (m, x) must emit
# multiple C bodies.
_UNRESOLVED_CALL_TARGETS: set = set()

# Set by v2_regen via set_rom_size(). Used by _emit_call to validate
# JSR/JSL target addresses against the LoROM mapping. Targets that
# resolve to RAM (pc < $8000) or beyond the ROM extent arise from the
# decoder following unreachable bytes past an RTS (the bytes happen to
# look like a JSL with garbage operands) — they would crash on real
# hardware too. Skipping the call emit avoids the need for hand-written
# stub functions in cfg.
_ROM_SIZE: int = 0
_REJECTED_CALL_TARGETS: set = set()


def set_rom_size(size: int) -> None:
    """Set the ROM size used for JSR/JSL target validation."""
    global _ROM_SIZE
    _ROM_SIZE = int(size)


# Per-call-site variant pin, populated from per-bank cfg
# `force_variant_at` directives. Keyed by the JSR/JSL site PC24 (the
# instruction's own address, NOT the target). When a Call site PC24 is
# present here, `_emit_call` bypasses the runtime 4-way (m, x) dispatch
# and hardcodes the single variant for the bound (m, x). Diagnostic
# only; see cfg_loader.BankCfg.force_variant_at for use rationale.
_FORCE_VARIANT_AT: Dict[int, Tuple[int, int]] = {}

# Source PC for the IR op currently being emitted. IR ops intentionally
# stay small and do not carry instruction addresses; emit_function passes
# the decoded instruction's pc24 through emit_op for trace attribution.
_CURRENT_SOURCE_PC24: int = 0


def _trace_pc_arg() -> str:
    return f"0x{_CURRENT_SOURCE_PC24 & 0xFFFFFF:06x}u"


# Per-RTS/RTL-site flag: populated by the post-lowering stack-delta
# analyser (see decoder.analyze_function_trampoline_returns). When a
# Return op's source_pc24 is in this set, `_emit_return` emits the
# PEI-trampoline dispatch path: pop the topmost frame from cpu->S,
# compute (PB:PC+1), tail-call cpu_dispatch_pc. For Returns NOT in
# the set, emit the standard `return _ps;`. The default — empty set —
# means every Return is treated as balanced (matches pre-fix behavior).
#
# Trampoline classification is based on an in-function dataflow pass
# that tracks `delta = sum(push_bytes - pop_bytes)` across the CFG.
# A Return reaches with delta != 0 from at least one path → flagged.
# Only the asm-instruction set is considered (PHP/PHA/PEI/.../PLA/PLP);
# JSR/JSL/RTS/RTL are treated as 0-delta (the codegen does NOT push
# their hardware frame onto cpu->S — see _emit_call / _emit_return).
_TRAMPOLINE_RETURNS: set = set()


def set_trampoline_returns(s: set) -> None:
    """Replace the per-Return-site trampoline classification set.

    Caller is responsible for populating with the union of all
    Returns flagged across every emitted bank in this regen run.
    Keys are the asm pc24 of the source RTS/RTL instruction (matches
    Return.source_pc24 stamped by lowering).
    """
    global _TRAMPOLINE_RETURNS
    _TRAMPOLINE_RETURNS = set(s) if s else set()


def add_trampoline_returns(s: set) -> None:
    """Union additional Return pc24s into the classification set.

    Used during the per-bank emit loop: each emit_bank pass classifies
    its own functions' Returns and contributes them to the global set.
    The auto-promote loop may re-emit banks with the union from prior
    passes — caller should snapshot before each pass and merge.
    """
    global _TRAMPOLINE_RETURNS
    _TRAMPOLINE_RETURNS = _TRAMPOLINE_RETURNS | set(s)


def take_trampoline_returns() -> set:
    """Return + clear the per-Return-site trampoline classification set.

    Used by the parallel emit path in v2_regen: each worker process
    accumulates trampoline classifications during emit_bank in its own
    Python module-level _TRAMPOLINE_RETURNS (workers are separate
    processes, not threads, so the global is worker-local). At end of
    each work item the worker drains via this function and returns the
    set to main; main unions across workers and reseeds for the next
    pass via set_trampoline_returns().
    """
    global _TRAMPOLINE_RETURNS
    out = _TRAMPOLINE_RETURNS
    _TRAMPOLINE_RETURNS = set()
    return out


def set_force_variant_at(d: Dict[int, Tuple[int, int]]) -> None:
    """Replace the per-site variant-pin map.

    v2_regen calls this once after collecting every cfg's
    force_variant_at entries. Pass an empty dict to clear. The
    site-PC24 key matches `Call.source_pc24`, stamped by lowering for
    BOTH direct JSR/JSL and indirect JSR (abs,X) sites.
    """
    global _FORCE_VARIANT_AT
    _FORCE_VARIANT_AT = dict(d) if d else {}


def take_rejected_call_targets() -> set:
    """Return + clear the set of Call targets rejected as out-of-ROM.
    Diagnostic for v2_regen + tests."""
    global _REJECTED_CALL_TARGETS
    out = _REJECTED_CALL_TARGETS
    _REJECTED_CALL_TARGETS = set()
    return out


def _is_invalid_lorom_call_target(addr_24: int) -> bool:
    """True when addr_24 cannot be a valid LoROM code target.

    Two structural rejections, both independent of any cfg directive:
      1. pc < $8000 — LoROM addresses $00-$7F:$0000-$7FFF are
         RAM/registers, never ROM code. (Mirrors at $80-$BF too.)
      2. (canonical_bank * $8000 + pc - $8000) >= rom_size — target
         byte is beyond the ROM image extent.

    With _ROM_SIZE unset (== 0) we only apply rule 1 to stay safe in
    unit-test contexts that don't load a ROM.
    """
    pc = addr_24 & 0xFFFF
    if pc < 0x8000:
        return True
    if _ROM_SIZE > 0:
        canon_bank = (addr_24 >> 16) & 0x7F
        offset = canon_bank * 0x8000 + (pc - 0x8000)
        if offset >= _ROM_SIZE:
            return True
    return False

# NOTE (2026-05-02): the `_UNRESOLVED_GOTO_TARGETS` machinery has been
# RETIRED. Auto-promoting arbitrary jump targets into separate C
# functions split asm routines across C scopes and stranded their
# PHB/PLB (and other stack-lifetime) invariants — root cause of DB=$C0
# at dispatcher entry, manifest as the title-screen-loop regression.
#
# Replacement: the decoder imports BRA/BRL/JMP-ABS/cond-branch targets
# that lie past cfg `end:` directly into the SAME function's CFG (see
# `_labeled_successors` + the end:-applies-to-fall-through-only rule
# in decoder.py). Auto-promote remains for genuine subroutine targets
# (JSR/JSL) only. See `record_unresolved_goto_target` placeholder
# below for the contract enforcement.


def _variant_suffix(m: int, x: int) -> str:
    """Return the `_M{m}X{x}` suffix used for per-variant function names.

    Centralised so emit_function, _emit_call, and the cross-tool
    sync_funcs_h regen all agree on the mangling. Suffix is universal
    in v2 — every gen function name carries it, every call site
    appends it. Hand-written entry-point shims (e.g. I_RESET in
    smw_rtl.c) rely on cfg-emitted aliases that drop the suffix for
    the cfg-default (m,x).
    """
    return f"_M{m & 1}X{x & 1}"


# All four (m, x) variants — used by every site that demands the full
# variant set (runtime dispatch emits one switch case per variant).
_MX_VARIANTS = ((0, 0), (0, 1), (1, 0), (1, 1))


# Per-target surviving-(m, x) set, installed by v2_regen after its
# emit-truth variant-prune pass. A function entered at a fixed width
# (e.g. an 8-bit-A routine) has wrong-width variants that decode to
# garbage — misaligned operands, phantom branches to non-code, calls
# to invalid LoROM addresses. When such a variant ALSO has a clean
# (marker-free) sibling variant, the clean sibling proves the bytes
# are real code at some width, so the garbage variant is provably the
# wrong width and is never validly reached. v2_regen prunes those
# variants (drops their bodies) and records the survivors here. The
# 4-way runtime (m, x) dispatch then emits cases ONLY for survivors,
# so no switch references a pruned (undefined) symbol and no garbage
# body emits a stub marker. Empty dict => pre-prune / unknown target
# => emit all four (the historical default). Keyed by 24-bit target
# address; the LoROM mirror ($00-$3F <-> $80-$BF) is consulted on a
# miss, mirroring _NAME_RESOLVER's aliasing.
_VALID_VARIANTS: Dict[int, frozenset] = {}
_VALID_VARIANTS_AUTHORITATIVE = False


def set_valid_variants(d, *, authoritative: bool = False) -> None:
    """Install the per-target surviving-(m, x) map from v2_regen's
    emit-truth prune pass. Pass an empty dict to clear (=> emit all
    four everywhere, the pre-prune behaviour)."""
    global _VALID_VARIANTS, _VALID_VARIANTS_AUTHORITATIVE
    _VALID_VARIANTS = d or {}
    _VALID_VARIANTS_AUTHORITATIVE = bool(authoritative)


def valid_variant_list(addr_24: int):
    """Return the (m, x) variants a dispatch switch should emit a case
    for at this call target, in canonical order. When v2_regen has
    recorded a pruned survivor set for the target (or its LoROM
    mirror), return that subset; otherwise return all four (pre-prune
    or cross-bank target with no recorded set)."""
    if not _VALID_VARIANTS and not _VALID_VARIANTS_AUTHORITATIVE:
        return _MX_VARIANTS
    a = addr_24 & 0xFFFFFF
    s = _VALID_VARIANTS.get(a)
    if s is None:
        bank = (a >> 16) & 0xFF
        if bank < 0x40 or 0x80 <= bank < 0xC0:
            s = _VALID_VARIANTS.get(a ^ 0x800000)
    if not s and not _VALID_VARIANTS_AUTHORITATIVE:
        return _MX_VARIANTS
    if not s:
        return ()
    return tuple(mx for mx in _MX_VARIANTS if mx in s)


def has_exact_variant(addr_24: int, m: int, x: int) -> bool:
    """Whether an exact AOT body exists for this architectural entry.

    An absent validity map is the legacy pre-analysis state and therefore
    assumes all four bodies will be emitted. Once a target has a survivor
    set, width identity is exact: a sibling is never a correctness fallback.
    """
    return ((m & 1), (x & 1)) in set(valid_variant_list(addr_24))


def variant_dispatch_case_lines(addr_24: int, base_name: str,
                                indent: str = "    ", pre_call=None,
                                lle_fallback=None):
    """Emit the case/default body for a runtime (m, x) dispatch switch.

    A case is emitted for all four (M, X) indices. Surviving variants call
    their exact body. A combination without an exact AOT body executes ROM
    through ``lle_fallback``; it never borrows a sibling decoded at a
    different operand width. The caller supplies the ABI-correct expression:
    call sites use ``interp_tier_run_call_frame`` after pushing the hardware
    frame, while tail transfers use ``interp_tier_dispatch_tail`` so an
    enclosing architectural interrupt retains its RTI boundary.

    `pre_call` is emitted only before an AOT variant call (e.g. tail-call
    return-context inheritance); the LLE bridge receives its own context.

    Every case/default is emitted as a SINGLE line beginning with
    `case N:` / `default:` — even with `pre_call`, the statements are inlined
    ahead of the variant call. This is REQUIRED: v2_regen's reference-taint
    prune (`_scan_variant_refs`) excludes a line from the direct-reference
    graph only when it starts with `case`/`default` (`_MX_DISPATCH_CASE_RE`).
    A multi-line case would put `_r = NAME(cpu);` on its own line, which the
    scanner then mis-counts as a DIRECT reference — and because these cases
    route to the (shrinking) survivor set, that perturbs the prune fixpoint
    into a non-terminating 1-clone-per-pass churn (observed regenerating
    ALttP). Single-line keeps the runtime (m, x) switch correctly out of the
    direct-reference graph (a switch case never dangles at runtime)."""
    survivors = list(valid_variant_list(addr_24))
    survivor_set = set(survivors)
    lines = []

    def emit(label: str, name: str, comment: str = ""):
        pre = (" ".join(pre_call) + " ") if pre_call else ""
        lines.append(f"{indent}{label}: {pre}_r = {name}(cpu); break;{comment}")

    def emit_lle(label: str, m: int, x: int):
        if not lle_fallback:
            raise ValueError(
                f"missing exact M{m}X{x} variant for ${addr_24:06X} "
                "without an ABI-specific LLE fallback")
        lines.append(
            f"{indent}{label}: _r = {lle_fallback}; break;"
            f"  /* exact M{m}X{x} -> authoritative LLE */")

    for m, x in _MX_VARIANTS:
        idx = (m << 1) | x
        if (m, x) in survivor_set:
            emit(f"case {idx}", f"{base_name}{_variant_suffix(m, x)}")
        else:
            emit_lle(f"case {idx}", m, x)
    if lle_fallback:
        lines.append(
            f"{indent}default: _r = {lle_fallback}; break;"
            "  /* masked M/X index should make this unreachable */")
    elif survivor_set == set(_MX_VARIANTS):
        lines.append(f"{indent}default: _r = RECOMP_RETURN_NORMAL; break;")
    else:
        raise ValueError(
            f"incomplete variant set for ${addr_24:06X} without LLE fallback")
    return lines


def set_name_resolver(name_map: Dict[int, str]) -> None:
    """Replace the call-target name resolver. Pass an empty dict to clear.

    Installs LoROM bank-mirror aliases automatically: for any entry whose
    bank is in $00-$3F or $80-$BF, the same name is registered for the
    mirrored bank (XOR 0x80). LoROM maps banks $00-$3F as the SAME
    physical bytes as $80-$BF; long-mode vector stubs (the canonical
    pattern in Mega Man X and similar) JML into the $80-$BF aliases of
    code that the cfg-author declared in $00-$3F. Without this aliasing,
    the recompiler treats $80:8007 as a distinct unresolved cross-ROM-bank
    Call target and emits a trap stub. With this aliasing, the JML
    resolves to the cfg-declared function name automatically. (2026-05-21)

    LoROM-only mapping. HiROM games (which use a different bank-mirror
    layout) would need a separate map; we don't ship a HiROM game yet,
    so the always-on LoROM alias is the conservative pre-policy. If a
    HiROM game arrives, gate this behind a ROM-mapping-aware flag set
    by v2_regen from the ROM internal header.
    """
    global _NAME_RESOLVER
    expanded: Dict[int, str] = {}
    for pc24, name in name_map.items():
        expanded[pc24] = name
        bank = (pc24 >> 16) & 0xFF
        if bank < 0x40 or 0x80 <= bank < 0xC0:
            mirror_bank = bank ^ 0x80
            mirror_pc24 = (mirror_bank << 16) | (pc24 & 0xFFFF)
            # Don't overwrite explicit entries for the mirror bank.
            if mirror_pc24 not in name_map:
                expanded[mirror_pc24] = name
    _NAME_RESOLVER = expanded


def take_unresolved_call_targets() -> set:
    """Return + clear the set of synthetic-name Call targets seen since
    the last call. Used by v2_regen for iterative auto-promote."""
    global _UNRESOLVED_CALL_TARGETS
    out = _UNRESOLVED_CALL_TARGETS
    _UNRESOLVED_CALL_TARGETS = set()
    return out


def take_unresolved_goto_targets() -> set:
    """Compatibility shim. The auto-promote-goto-targets pass has been
    retired (2026-05-02) in favor of the inline-cross-fn-blocks model.
    Returns an empty set so v2_regen's drain loop terminates immediately
    on the first pass.

    Old callers of `record_unresolved_goto_target` are gone — emit_function
    now emits a LOUD `return; /* unresolvable cross-fn goto */` for the
    handful of jumps that can't be imported (cross-bank, out-of-range),
    without recording for promotion."""
    return set()


def get_name_for_pc(pc24: int):
    """Look up the friendly C function name registered for a 24-bit address.
    Returns None if no cfg/ingester directive named the address.

    Used by emit_function's tail-call resolver: when a goto target lies
    past the current function's `end:` boundary AND lands on a known
    function entry in the same bank, emit a tail call to that function
    rather than the unresolvable-goto trap. The asm idiom (a function
    falling through into its declared successor sibling) becomes a real
    C tail call, preserving both the function-boundary semantics the
    name-map declared AND the asm's fall-through semantic."""
    return _NAME_RESOLVER.get(pc24 & 0xFFFFFF)


def register_call_demand(pc24: int, m: int, x: int) -> None:
    """Register that some emitted gen code needs a callable
    `<name><variant_suffix>(cpu)` at the given (pc24, m, x). v2_regen's
    auto-promote pass diffs this set against the variants actually
    emitted and clones the cfg entry to cover any missing one.

    Used by emit_function's tail-call resolver — the synthesized tail
    call to a sibling function at the fall-through-past-end: site must
    have its (m, x) variant emitted or the C linker will fail."""
    _UNRESOLVED_CALL_TARGETS.add((pc24 & 0xFFFFFF, m & 1, x & 1))


from v2 import widths  # noqa: E402
from v2 import emitter_helpers  # noqa: E402
from v2.ir import (  # noqa: E402
    IROp, IRBlock,
    Read, Write, ReadReg, WriteReg, ConstI,
    Alu, AluOp, Shift, ShiftOp, IncReg, IncMem,
    BitTest, BitSetMem, BitClearMem,
    SetFlag, SetNZ, RepFlags, SepFlags, XCE,
    Push, Pull, PushReg, PullReg, BlockMove,
    CondBranch, Goto, IndirectGoto, Call, Return,
    Transfer, XBA, Nop, Break, Stop, PushEffectiveAddress,
    Reg, SegRef, SegKind, Value,
)
from snes65816 import INDIR, INDIR_X  # noqa: E402


# ── Helpers ─────────────────────────────────────────────────────────────────

def _v(value: Value) -> str:
    """Format a Value as its C local name."""
    return f"_v{value.vid}"


def _ctype(width: int) -> str:
    return "uint8" if width == 1 else "uint16"


# Reg → CpuState field expression.
#
# Reg.B is intentionally a DERIVED expression, not a struct field. The 65816
# B register is the high byte of the 16-bit accumulator and ALWAYS equals
# `(A >> 8) & 0xFF`. Earlier versions had a `cpu->B` shadow field; that
# field went stale every time a 16-bit LDA wrote `cpu->A` without syncing
# the shadow, and XBA-after-LDA-in-m=0 swapped in stale bytes (SMW
# Layer-3 stripe corruption, fixed in commit 6c04c94, then removed
# entirely in this commit).
_REG_FIELD = {
    Reg.A: "cpu->A", Reg.B: "((uint8)((cpu->A >> 8) & 0xFF))",
    Reg.X: "cpu->X", Reg.Y: "cpu->Y",
    Reg.S: "cpu->S", Reg.D: "cpu->D",
    Reg.DB: "cpu->DB", Reg.PB: "cpu->PB",
    Reg.P: "cpu->P",
    Reg.M: "cpu->m_flag", Reg.XF: "cpu->x_flag", Reg.E: "cpu->emulation",
    Reg.N: "cpu->_flag_N", Reg.V: "cpu->_flag_V",
    Reg.ZF: "cpu->_flag_Z", Reg.C: "cpu->_flag_C",
    Reg.I: "cpu->_flag_I", Reg.DF: "cpu->_flag_D",
}


def _reg(r: Reg) -> str:
    return _REG_FIELD[r]


# ── SegRef → C address expressions ──────────────────────────────────────────

def _segref_addr_expr(seg: SegRef) -> tuple:
    """Resolve a SegRef into (bank_expr, addr_expr) C strings.

    bank_expr / addr_expr reference cpu state where appropriate. The
    caller passes them to cpu_read* / cpu_write* primitives.
    """
    idx = ""
    if seg.index == Reg.X:
        idx = " + cpu->X"
    elif seg.index == Reg.Y:
        idx = " + cpu->Y"

    k = seg.kind
    if k == SegKind.DIRECT:
        # Direct-page addressing ALWAYS resolves in bank $00 (65816 contract):
        # effective = (D + dp + index) & 0xFFFF, bank 0. The old "0x7E" bank
        # only happened to work when the effective address stayed below $2000,
        # where $7E:0000-1FFF aliases $00:0000-1FFF in the WRAM low mirror; once
        # D+index reaches $2000-$5FFF the bank decides everything — $00:21xx is a
        # PPU/hw register, $7E:21xx is plain WRAM. DKC2 set_ppu_registers does
        # `STA $00,x` with X = a PPU register number ($2105 …); the $7E emit sent
        # every PPU register write into WRAM, so the PPU config never applied →
        # forced-blank stall. The (uint16) addr_expr already models the bank-0
        # 16-bit wrap; only the bank was wrong.
        return ("0x00", f"(uint16)(cpu->D + {seg.offset:#06x}{idx})")
    if k == SegKind.ABS_BANK:
        if seg.index is None:
            return ("cpu->DB", f"(uint16)({seg.offset:#06x})")
        # Indexed Absolute (ABS_X / ABS_Y): hardware computes a 24-bit
        # effective address `DB:offset + index`, with the carry from
        # `offset + index` propagating INTO THE BANK. Truncating to
        # uint16 (the old emit) silently lost the bank carry — root
        # cause of the Zelda intro submodule-reset bug 2026-05-17:
        # `STA $7E2000,X` with X=0xFF10 should land at $7F:1F10, but
        # the old emit stored at $7E:1F10, clobbering $7E:1F11 which
        # holds submodule_index. Compute the 24-bit effective inline
        # and let the C compiler CSE the duplicate sub-expression
        # between bank_expr and addr_expr.
        idx_reg = "cpu->X" if seg.index == Reg.X else "cpu->Y"
        eff24 = (f"(((uint32)cpu->DB << 16) + (uint32){seg.offset:#06x}"
                 f" + (uint32){idx_reg})")
        return (f"(uint8)(({eff24}) >> 16)", f"(uint16)({eff24})")
    if k == SegKind.LONG:
        bank = seg.bank if seg.bank is not None else 0
        if seg.index is None:
            return (f"{bank:#04x}", f"(uint16)({seg.offset:#06x})")
        # Indexed Absolute Long (LONG_X / LONG_Y): same bank-carry rule.
        base24 = (bank << 16) | (seg.offset & 0xFFFF)
        idx_reg = "cpu->X" if seg.index == Reg.X else "cpu->Y"
        eff24 = f"((uint32){base24:#08x} + (uint32){idx_reg})"
        return (f"(uint8)(({eff24}) >> 16)", f"(uint16)({eff24})")
    if k == SegKind.STACK:
        return ("0x00", f"(uint16)(cpu->S + {seg.offset:#06x})")
    if k == SegKind.DP_INDIRECT:
        # ((D + dp) word) (+ Y if indirect-Y), DB-bank.
        ptr_addr = f"(uint16)(cpu->D + {seg.offset:#06x})"
        return ("cpu->DB", f"(uint16)(cpu_read16(cpu, 0x00, {ptr_addr}){idx})")
    if k == SegKind.DP_INDIRECT_LONG:
        # ((D + dp) long) (+ Y).  The indexed form adds Y to the full
        # 24-bit pointer, including carry into the bank.  Keeping the pointer's
        # bank byte while truncating only (word + Y) misreads every exact bank
        # crossing.  DKC2's decompressor exposed this with [$34],Y where
        # $DF:D537 + $2AC9 must read $E0:0000, not $DF:0000.
        ptr_addr = f"(uint16)(cpu->D + {seg.offset:#06x})"
        bank_expr = f"cpu_read8(cpu, 0x00, (uint16)({ptr_addr} + 2))"
        if seg.index is None:
            return (bank_expr, f"cpu_read16(cpu, 0x00, {ptr_addr})")
        idx_reg = "cpu->X" if seg.index == Reg.X else "cpu->Y"
        base24 = (f"(((uint32){bank_expr} << 16) | "
                  f"(uint32)cpu_read16(cpu, 0x00, {ptr_addr}))")
        eff24 = f"({base24} + (uint32){idx_reg})"
        return (f"(uint8)(({eff24}) >> 16)", f"(uint16)({eff24})")
    if k == SegKind.ABS_INDIRECT:
        return ("cpu->PB",
                f"cpu_read16(cpu, cpu->PB, (uint16){seg.offset:#06x})")
    if k == SegKind.ABS_INDIRECT_X:
        return ("cpu->PB",
                f"cpu_read16(cpu, cpu->PB, (uint16)({seg.offset:#06x} + cpu->X))")
    if k == SegKind.ABS_INDIRECT_LONG:
        addr = f"(uint16){seg.offset:#06x}"
        return (f"cpu_read8(cpu, 0x00, (uint16)({addr} + 2))",
                f"cpu_read16(cpu, 0x00, {addr})")
    if k == SegKind.DP_INDIRECT_X:
        ptr_addr = f"(uint16)(cpu->D + {seg.offset:#06x} + cpu->X)"
        return ("cpu->DB", f"cpu_read16(cpu, 0x00, {ptr_addr})")
    if k == SegKind.STACK_REL_INDIRECT_Y:
        ptr_addr = f"(uint16)(cpu->S + {seg.offset:#06x})"
        return ("cpu->DB",
                f"(uint16)(cpu_read16(cpu, 0x00, {ptr_addr}) + cpu->Y)")
    raise ValueError(f"unsupported SegKind {k}")


# ── Per-op handlers ─────────────────────────────────────────────────────────

def _emit_read(op: Read) -> List[str]:
    bank, addr = _segref_addr_expr(op.seg)
    return [f"{widths.ctype(op.width)} {_v(op.out)} = "
            f"{widths.read_fn(op.width)}(cpu, {bank}, {addr});"]


def _emit_write(op: Write) -> List[str]:
    bank, addr = _segref_addr_expr(op.seg)
    return [f"{widths.write_fn(op.width)}(cpu, {bank}, {addr}, {_v(op.src)});"]


def _emit_readreg(op: ReadReg) -> List[str]:
    """Emit a 16-bit read of A/X/Y/etc. For A, X, Y we route through
    cpu_read_{a,x,y}16 helpers — same value, but the helper name carries
    the hardware contract and the lint can spot bypass attempts. For
    Reg.B the _REG_FIELD mapping returns the derived `(cpu->A >> 8)`
    expression directly.

    NOTE: This always emits a 16-bit uint. Callers that want 8-bit must
    mask via widths.masked(). Width-aware callers (e.g. ALU, BIT, INC)
    already do this at op-level. The 8-bit-direct helpers
    (cpu_read_a8/x8/y8) exist for hand-written runtime code, not v2
    codegen output."""
    if op.reg == Reg.A:
        return [f"uint16 {_v(op.out)} = cpu_read_a16(cpu);"]
    if op.reg == Reg.X:
        return [f"uint16 {_v(op.out)} = cpu_read_x16(cpu);"]
    if op.reg == Reg.Y:
        return [f"uint16 {_v(op.out)} = cpu_read_y16(cpu);"]
    return [f"uint16 {_v(op.out)} = (uint16){_reg(op.reg)};"]


def _emit_writereg(op: WriteReg) -> List[str]:
    """Width-respecting write into A / X / Y, routed through the typed
    helpers in cpu_state.h. The helpers encapsulate the hardware
    contract:

    - cpu_write_a_m: in m=1, preserve A.high (= B). In m=0, full
      16-bit replace. Callers don't have to remember the asymmetry
      vs X/Y.

    - cpu_write_x_x / cpu_write_y_x: in x=1, ZERO the high byte (hw
      contract). In x=0, full 16-bit replace. The historical "8-bit
      X/Y zero-extend" bug class (snesrecomp 6o, b39e99b) was
      contributors copy-pasting the A shape onto X/Y and letting
      stale 16-bit residuals leak through indexed reads. The helper
      makes that copy-paste impossible — the hardware contract is
      part of the function name.

    Other registers (S, D, DB, PB, P) use direct field assignment;
    they have a single canonical width.
    """
    src = _v(op.src)
    if op.reg == Reg.A:
        return [f"cpu_write_a_m(cpu, (uint16)({src}));"]
    if op.reg == Reg.X:
        return [f"cpu_write_x_x(cpu, (uint16)({src}));"]
    if op.reg == Reg.Y:
        return [f"cpu_write_y_x(cpu, (uint16)({src}));"]
    return [f"{_reg(op.reg)} = {src};"]


def _emit_consti(op: ConstI) -> List[str]:
    return [f"{_ctype(op.width)} {_v(op.out)} = {op.value:#x};"]


def _emit_addsub(tname, lhs_m, rhs_m, out_v, width, is_sub) -> List[str]:
    """ADC / SBC with a decimal (BCD) branch. The binary branch is the proven
    path (unchanged); the decimal branch mirrors interp816 (LakeSnes) nibble-wise
    exactly so the cpu_diff differential stays green. V is computed at the same
    point interp816 does (after the nibble add, before the final >9.. fixup)."""
    ct = widths.ctype(width)
    L, R = lhs_m, rhs_m
    lines = [f"{ct} {out_v};", "if (cpu->_flag_D) {"]
    if not is_sub:
        if width == 1:
            bcd = [
                f"int _bcd = ({L} & 0xf) + ({R} & 0xf) + cpu->_flag_C;",
                "if (_bcd > 0x9) _bcd = ((_bcd + 0x6) & 0xf) + 0x10;",
                f"_bcd = ({L} & 0xf0) + ({R} & 0xf0) + _bcd;",
                f"cpu->_flag_V = (({L} & 0x80) == ({R} & 0x80)) && (({R} & 0x80) != (_bcd & 0x80)) ? 1 : 0;",
                "if (_bcd > 0x9f) _bcd += 0x60;",
                "cpu->_flag_C = (_bcd > 0xff) ? 1 : 0;",
                f"{out_v} = (uint8)(_bcd & 0xff);",
            ]
        else:
            bcd = [
                f"int _bcd = ({L} & 0xf) + ({R} & 0xf) + cpu->_flag_C;",
                "if (_bcd > 0x9) _bcd = ((_bcd + 0x6) & 0xf) + 0x10;",
                f"_bcd = ({L} & 0xf0) + ({R} & 0xf0) + _bcd;",
                "if (_bcd > 0x9f) _bcd = ((_bcd + 0x60) & 0xff) + 0x100;",
                f"_bcd = ({L} & 0xf00) + ({R} & 0xf00) + _bcd;",
                "if (_bcd > 0x9ff) _bcd = ((_bcd + 0x600) & 0xfff) + 0x1000;",
                f"_bcd = ({L} & 0xf000) + ({R} & 0xf000) + _bcd;",
                f"cpu->_flag_V = (({L} & 0x8000) == ({R} & 0x8000)) && (({R} & 0x8000) != (_bcd & 0x8000)) ? 1 : 0;",
                "if (_bcd > 0x9fff) _bcd += 0x6000;",
                "cpu->_flag_C = (_bcd > 0xffff) ? 1 : 0;",
                f"{out_v} = (uint16)_bcd;",
            ]
    else:
        if width == 1:
            bcd = [
                f"int _bcv = ({R} ^ 0xff) & 0xff;",
                f"int _bcd = ({L} & 0xf) + (_bcv & 0xf) + cpu->_flag_C;",
                "if (_bcd < 0x10) _bcd = (_bcd - 0x6) & ((_bcd - 0x6 < 0) ? 0xf : 0x1f);",
                f"_bcd = ({L} & 0xf0) + (_bcv & 0xf0) + _bcd;",
                f"cpu->_flag_V = (({L} & 0x80) == (_bcv & 0x80)) && ((_bcv & 0x80) != (_bcd & 0x80)) ? 1 : 0;",
                "if (_bcd < 0x100) _bcd -= 0x60;",
                "cpu->_flag_C = (_bcd > 0xff) ? 1 : 0;",
                f"{out_v} = (uint8)(_bcd & 0xff);",
            ]
        else:
            bcd = [
                f"int _bcv = ({R} ^ 0xffff) & 0xffff;",
                f"int _bcd = ({L} & 0xf) + (_bcv & 0xf) + cpu->_flag_C;",
                "if (_bcd < 0x10) _bcd = (_bcd - 0x6) & ((_bcd - 0x6 < 0) ? 0xf : 0x1f);",
                f"_bcd = ({L} & 0xf0) + (_bcv & 0xf0) + _bcd;",
                "if (_bcd < 0x100) _bcd = (_bcd - 0x60) & ((_bcd - 0x60 < 0) ? 0xff : 0x1ff);",
                f"_bcd = ({L} & 0xf00) + (_bcv & 0xf00) + _bcd;",
                "if (_bcd < 0x1000) _bcd = (_bcd - 0x600) & ((_bcd - 0x600 < 0) ? 0xfff : 0x1fff);",
                f"_bcd = ({L} & 0xf000) + (_bcv & 0xf000) + _bcd;",
                f"cpu->_flag_V = (({L} & 0x8000) == (_bcv & 0x8000)) && ((_bcv & 0x8000) != (_bcd & 0x8000)) ? 1 : 0;",
                "if (_bcd < 0x10000) _bcd -= 0x6000;",
                "cpu->_flag_C = (_bcd > 0xffff) ? 1 : 0;",
                f"{out_v} = (uint16)_bcd;",
            ]
    lines += ["  " + s for s in bcd]
    lines.append("} else {")
    if not is_sub:
        bin_lines = [
            f"uint32 {tname} = (uint32){L} + (uint32){R} + cpu->_flag_C;",
            f"{out_v} = ({ct}){tname};",
            widths.set_carry_from_overflow(tname, width, "add"),
            widths.set_v_adc(L, R, out_v, width),
        ]
    else:
        bin_lines = [
            f"uint32 {tname} = (uint32){L} - (uint32){R} - (1 - cpu->_flag_C);",
            f"{out_v} = ({ct}){tname};",
            widths.set_carry_from_overflow(tname, width, "sub"),
            widths.set_v_sbc(L, R, out_v, width),
        ]
    lines += ["  " + s for s in bin_lines]
    lines.append("}")
    return lines


def _emit_alu(op: Alu) -> List[str]:
    """Emit an ALU op. Internal `_t` temp is named per-output-vid (or
    per-lhs-vid for CMP which has no out) so multiple ALU ops in the
    same C function don't conflict on `_t`.

    Width contract — see `widths.py` (canonical width-literal home):
    ReadReg always emits a uint16 read of cpu->A/X/Y, so width=1 ALU
    ops MUST mask both operands via `widths.masked` before computing
    carry/borrow/sign. Otherwise the high byte (B-register for A, or
    stale hw-zero for X/Y) leaks into the result.
    """
    if op.out is not None:
        tname = f"_t{op.out.vid}"
    else:
        tname = f"_tc{op.lhs.vid}_{op.rhs.vid}"  # CMP: no out

    lines = []
    lhs_m = widths.masked(_v(op.lhs), op.width)
    rhs_m = widths.masked(_v(op.rhs), op.width)
    if op.op == AluOp.ADD:
        # ADC always writes A; decimal-aware (binary path unchanged).
        lines.extend(_emit_addsub(tname, lhs_m, rhs_m, _v(op.out), op.width, False))
    elif op.op == AluOp.SUB:
        lines.extend(_emit_addsub(tname, lhs_m, rhs_m, _v(op.out), op.width, True))
    elif op.op == AluOp.AND:
        lines.append(
            f"{widths.ctype(op.width)} {_v(op.out)} = "
            f"({widths.ctype(op.width)})({_v(op.lhs)} & {_v(op.rhs)});"
        )
    elif op.op == AluOp.OR:
        lines.append(
            f"{widths.ctype(op.width)} {_v(op.out)} = "
            f"({widths.ctype(op.width)})({_v(op.lhs)} | {_v(op.rhs)});"
        )
    elif op.op == AluOp.XOR:
        lines.append(
            f"{widths.ctype(op.width)} {_v(op.out)} = "
            f"({widths.ctype(op.width)})({_v(op.lhs)} ^ {_v(op.rhs)});"
        )
    elif op.op == AluOp.CMP:
        lines.append(
            f"uint32 {tname} = (uint32){lhs_m} - (uint32){rhs_m};"
        )
        lines.append(f"cpu->_flag_C = ({lhs_m} >= {rhs_m}) ? 1 : 0;")
        # CMP doesn't update cpu->P here either — historical
        # behavior matched _emit_shift; both now route through helpers.
        lines.extend(widths.set_nz_no_p(f"({widths.ctype(op.width)}){tname}", op.width))
        return lines

    if op.out is not None:
        # Result is already in width-typed _v(op.out), so set N/Z from
        # it. Skip cpu->P update for ALU (preserves historical
        # behavior; SEP/REP at next mode boundary will resync via
        # cpu_mirrors_to_p as fixed in 44c96a7).
        lines.extend(widths.set_nz_no_p(_v(op.out), op.width))
    return lines


def _emit_shift(op: Shift) -> List[str]:
    """Width contract — see `widths.py`. The pre-DRY emitter forgot
    the `widths.masked` step on src for several years (b39e99b/8f9369d
    fixed it reactively per op). Now uniform via helpers."""
    src_m = widths.masked(_v(op.src), op.width)
    sign = widths.sign_bit(op.width)
    out_v = _v(op.out)
    out_t = widths.ctype(op.width)
    if op.op == ShiftOp.ASL:
        return [
            f"{out_t} {out_v} = ({out_t})({src_m} << 1);",
            widths.set_carry_from_bit(src_m, sign),
        ] + widths.set_nz_no_p(out_v, op.width)
    if op.op == ShiftOp.LSR:
        return [
            f"{out_t} {out_v} = ({out_t})({src_m} >> 1);",
            widths.set_carry_from_bit(src_m, "1"),
        ] + widths.set_nz_no_p(out_v, op.width)
    if op.op == ShiftOp.ROL:
        return [
            f"{out_t} {out_v} = "
            f"({out_t})(({src_m} << 1) | cpu->_flag_C);",
            widths.set_carry_from_bit(src_m, sign),
        ] + widths.set_nz_no_p(out_v, op.width)
    if op.op == ShiftOp.ROR:
        return [
            f"{out_t} {out_v} = "
            f"({out_t})(({src_m} >> 1) | "
            f"((uint{op.width*8})cpu->_flag_C << {op.width * 8 - 1}));",
            widths.set_carry_from_bit(src_m, "1"),
        ] + widths.set_nz_no_p(out_v, op.width)
    raise ValueError(f"unhandled Shift op {op.op}")


def _emit_increg(op: IncReg) -> List[str]:
    field = _reg(op.reg)
    delta = "1" if op.delta == +1 else "-1"
    # 65816 width semantics:
    #   INC A: width follows M (0=16-bit, 1=8-bit)
    #   INX / INY / DEX / DEY: width follows X (0=16-bit, 1=8-bit)
    # A high byte is the B register; INC A in m=1 must NOT carry into B.
    # X/Y high byte is HARDWARE-ZERO in x=1 mode (SEP #$10 zeros it at
    # the flag transition; subsequent 8-bit ops can't physically write
    # to it). Old codegen preserved X/Y high across 8-bit increments,
    # which is wrong: stale 16-bit residuals leaked through. Indexed
    # addressing then read from base + (stale_high<<8 | new_low) and
    # NMI's LoadStripeImage spun for 30k+ iterations on garbage stripe
    # data. Fixed 2026-04-30.
    if op.reg == Reg.A:
        # m=1: 8-bit INC, preserve B (high byte). m=0: 16-bit INC.
        lines = [f"if (cpu->m_flag) {{",
                 f"  uint8 _lo8 = ({widths.low_byte(field)}) + ({delta});",
                 f"  {field} = {widths.preserve_high(field, '_lo8')};"]
        lines.extend(f"  {s}" for s in widths.set_nz_no_p("_lo8", 1))
        lines.append("} else {")
        lines.append(f"  {field} = (uint16)(({field}) + ({delta}));")
        lines.extend(f"  {s}" for s in widths.set_nz_no_p(field, 2))
        lines.append("}")
        return lines
    if op.reg in (Reg.X, Reg.Y):
        # x=1: 8-bit INC, ZERO high (hw contract). x=0: 16-bit INC.
        lines = [f"if (cpu->x_flag) {{",
                 f"  uint8 _lo8 = ({widths.low_byte(field)}) + ({delta});",
                 f"  {field} = {widths.zero_extend_lo('_lo8')};"
                 f"  /* x=1 zeros high byte (hw contract) */"]
        lines.extend(f"  {s}" for s in widths.set_nz_no_p("_lo8", 1))
        lines.append("} else {")
        lines.append(f"  {field} = (uint16)(({field}) + ({delta}));")
        lines.extend(f"  {s}" for s in widths.set_nz_no_p(field, 2))
        lines.append("}")
        return lines
    # Other registers (D, S) — always 16-bit native.
    return [f"{field} = ({field}) + ({delta});"] + widths.set_nz_no_p(field, 2)


def _emit_incmem(op: IncMem) -> List[str]:
    """INC/DEC memory: result = mem + delta (no carry-in); set Z/N from
    result; leave C and V untouched. 65816 hw spec for INC/DEC abs/dp.
    Distinct from ADC/SBC (Alu.ADD/SUB) which DO carry-in and update C/V."""
    bank, addr = _segref_addr_expr(op.seg)
    delta = "+1" if op.delta == +1 else "-1"
    ctype = widths.ctype(op.width)
    lines = [
        "{",
        f"  {ctype} _im = {widths.read_fn(op.width)}(cpu, {bank}, {addr});",
        f"  _im = ({ctype})(_im {delta});",
        f"  {widths.write_fn(op.width)}(cpu, {bank}, {addr}, _im);",
    ]
    lines.extend(f"  {s}" for s in widths.set_nz_no_p("_im", op.width))
    lines.append("}")
    return lines


def _emit_bittest(op: BitTest) -> List[str]:
    """BIT instruction: Z from A AND mem, N/V from mem bits.
    A is masked via cast through ctype to avoid B-register leaking.
    N/V bits are width-relative — see `widths.sign_bit`/`overflow_bit`."""
    sign = widths.sign_bit(op.width)
    overflow = widths.overflow_bit(op.width)
    ctype = widths.ctype(op.width)
    a_m = widths.masked("cpu->A", op.width)
    operand_m = widths.masked(_v(op.operand), op.width)
    if getattr(op, "imm", False):
        # BIT #imm ($89): immediate form sets ONLY Z (N/V unchanged).
        return [
            "{",
            f"  {ctype} _bt = ({ctype})({a_m} & {operand_m});",
            f"  cpu->_flag_Z = (_bt == 0) ? 1 : 0;",
            "}",
        ]
    return [
        "{",
        f"  {ctype} _bt = ({ctype})({a_m} & {operand_m});",
        f"  cpu->_flag_Z = (_bt == 0) ? 1 : 0;",
        f"  cpu->_flag_N = (({operand_m} & {sign}) != 0) ? 1 : 0;",
        f"  cpu->_flag_V = (({operand_m} & {overflow}) != 0) ? 1 : 0;",
        "}",
    ]


def _emit_bitsetmem(op: BitSetMem) -> List[str]:
    bank, addr = _segref_addr_expr(op.seg)
    ctype = widths.ctype(op.width)
    return [
        "{",
        f"  {ctype} _m = {widths.read_fn(op.width)}(cpu, {bank}, {addr});",
        f"  cpu->_flag_Z = ((_m & cpu->A) == 0) ? 1 : 0;",
        f"  {widths.write_fn(op.width)}(cpu, {bank}, {addr}, ({ctype})(_m | cpu->A));",
        "}",
    ]


def _emit_bitclearmem(op: BitClearMem) -> List[str]:
    bank, addr = _segref_addr_expr(op.seg)
    ctype = widths.ctype(op.width)
    return [
        "{",
        f"  {ctype} _m = {widths.read_fn(op.width)}(cpu, {bank}, {addr});",
        f"  cpu->_flag_Z = ((_m & cpu->A) == 0) ? 1 : 0;",
        f"  {widths.write_fn(op.width)}(cpu, {bank}, {addr}, ({ctype})(_m & ~cpu->A));",
        "}",
    ]


def _emit_setflag(op: SetFlag) -> List[str]:
    # Update both the per-flag mirror and the canonical cpu->P bit,
    # so subsequent PHP / direct cpu->P reads see a consistent byte.
    flag_to_p_mask = {
        Reg.C: "0x01", Reg.ZF: "0x02", Reg.I: "0x04", Reg.DF: "0x08",
        Reg.XF: "0x10", Reg.M: "0x20", Reg.V: "0x40", Reg.N: "0x80",
    }
    mask = flag_to_p_mask.get(op.flag)
    lines = [f"{_reg(op.flag)} = {op.value};"]
    if mask is not None:
        if op.value:
            lines.append(f"cpu->P = (uint8)(cpu->P | {mask});")
        else:
            lines.append(f"cpu->P = (uint8)(cpu->P & ~{mask});")
    return lines


def _emit_setnz(op) -> List[str]:
    """Update N/Z mirrors and cpu->P bits based on op.src's bits."""
    return widths.set_nz(widths.masked(_v(op.src), op.width), op.width)


def _emit_repflags(op: RepFlags) -> List[str]:
    # mirrors_to_p BEFORE modifying P (44c96a7) — see emitter_helpers.
    return ["{"] + [f"  {s}" for s in
                    emitter_helpers.modify_p_via_mirrors(op.mask, "rep")] + ["}"]


def _emit_sepflags(op: SepFlags) -> List[str]:
    return ["{"] + [f"  {s}" for s in
                    emitter_helpers.modify_p_via_mirrors(op.mask, "sep")] + ["}"]


def _emit_xce(op: XCE) -> List[str]:
    return [
        "{",
        "  uint8 _old_p = cpu->P;",
        "  uint8 _t = cpu->emulation;",
        "  cpu->emulation = cpu->_flag_C;",
        "  cpu->_flag_C = _t;",
        "  if (cpu->emulation) { cpu->m_flag = 1; cpu->x_flag = 1; cpu_mirrors_to_p(cpu); }",
        "  cpu_trace_px_record(cpu, 0, 7 /*XCE*/, _old_p, cpu->P);",
        "}",
    ]


def _emit_xba(op: XBA) -> List[str]:
    """XBA: exchange the high and low bytes of the 16-bit accumulator.
    Always 8-bit byte swap regardless of m_flag.

    Operates ENTIRELY on cpu->A — there is no separate B shadow field
    to keep in sync. The byte the 65816 calls "B" is just the high byte
    of A; it changes whenever any operation mutates A's high half (LDA
    in m=0, TCD/TDC pair manipulation, etc.). A separate `cpu->B` shadow
    invited stale-read bugs (the SMW stripe-image header parse used
    `LDA [_0],Y / XBA / AND #$3FFF / TAX`, and a stale shadow made it
    mis-derive the byte-count — visible as Layer-3 attract-demo
    scramble). The shadow has been removed; XBA must not reintroduce
    a dependency on it.

    Z/N flags are set from the new A.low byte (= old A.high), per the
    65816 manual.
    """
    return [
        "{",
        "  uint16 _old = cpu->A;",
        "  cpu->A = (uint16)(((_old & 0xFF) << 8) | ((_old >> 8) & 0xFF));",
        "  cpu->_flag_Z = ((cpu->A & 0xFF) == 0) ? 1 : 0;",
        "  cpu->_flag_N = ((cpu->A & 0x80) != 0) ? 1 : 0;",
        "}",
    ]


def _emit_pushreg(op: PushReg) -> List[str]:
    field = _reg(op.reg)
    # Push width: A/X/Y follow m/x_flag at runtime; D is always 16-bit;
    # P/DB/PB are always 1 byte.
    if op.reg == Reg.P:
        # PHP also records the P-mutation ring snapshot.
        return [
            "cpu_mirrors_to_p(cpu);",
            *emitter_helpers.stack_op_traced(
                "CPU_STACK_OP_PHP", -1,
                emitter_helpers.push_byte(f"(uint8)({field})")),
            "cpu_trace_event(cpu, 0, CPU_TR_PHP, cpu->P, 0);",
            "cpu_trace_px_record(cpu, 0, 4 /*PHP*/, cpu->P, cpu->P);",
        ]
    if op.reg == Reg.DB:
        return emitter_helpers.stack_op_traced(
            "CPU_STACK_OP_PHB", -1,
            emitter_helpers.push_byte(f"(uint8)({field})")
        ) + [
            "cpu_trace_event(cpu, 0, CPU_TR_PHB, cpu->DB, cpu->DB);",
        ]
    if op.reg == Reg.PB:
        # PHK pushes the program-bank K. Stale PB here is the suspected
        # root cause of bogus DB after PLB.
        return emitter_helpers.stack_op_traced(
            "CPU_STACK_OP_PHK", -1,
            emitter_helpers.push_byte(f"(uint8)({field})")
        ) + [
            "cpu_trace_event(cpu, 0, CPU_TR_PHK, cpu->PB, cpu->PB);",
        ]
    if op.reg == Reg.D:
        return emitter_helpers.stack_op_traced(
            "CPU_STACK_OP_PHD", -2, emitter_helpers.push_word(field))
    # A/X/Y: width depends on M/X flag.
    #
    # Prefer the decoder's per-instruction static M/X (`op.static_m` /
    # `op.static_x`) over runtime `cpu->m_flag` / `cpu->x_flag`. When
    # static is known the decoder has already proven the width at this
    # PC; emitting a runtime branch would let caller-side flag
    # corruption desync this push from a later pull bracketed by REP/
    # SEP. See `_push_reg` in lowering.py for the Iggy-platform repro
    # that motivates this. Static `None` (legacy callers, manual
    # construction in tests) falls back to the runtime branch.
    if op.reg == Reg.A:
        if op.static_m == 1:
            return [
                "{ uint16 _old_s = cpu->S;",
                *(f"  {s}" for s in emitter_helpers.push_byte(widths.low_byte(field))),
                "  cpu_trace_stack_op(cpu, 0, CPU_STACK_OP_PHA, _old_s, -1); }",
            ]
        if op.static_m == 0:
            return [
                "{ uint16 _old_s = cpu->S;",
                *(f"  {s}" for s in emitter_helpers.push_word(field)),
                "  cpu_trace_stack_op(cpu, 0, CPU_STACK_OP_PHA, _old_s, -2); }",
            ]
        return [
            "{ uint16 _old_s = cpu->S;",
            "  if (cpu->m_flag) {",
            *(f"    {s}" for s in emitter_helpers.push_byte(widths.low_byte(field))),
            "    cpu_trace_stack_op(cpu, 0, CPU_STACK_OP_PHA, _old_s, -1);",
            "  } else {",
            *(f"    {s}" for s in emitter_helpers.push_word(field)),
            "    cpu_trace_stack_op(cpu, 0, CPU_STACK_OP_PHA, _old_s, -2);",
            "  } }",
        ]
    if op.reg in (Reg.X, Reg.Y):
        op_id = "CPU_STACK_OP_PHX" if op.reg == Reg.X else "CPU_STACK_OP_PHY"
        if op.static_x == 1:
            return [
                "{ uint16 _old_s = cpu->S;",
                *(f"  {s}" for s in emitter_helpers.push_byte(widths.low_byte(field))),
                f"  cpu_trace_stack_op(cpu, 0, {op_id}, _old_s, -1); }}",
            ]
        if op.static_x == 0:
            return [
                "{ uint16 _old_s = cpu->S;",
                *(f"  {s}" for s in emitter_helpers.push_word(field)),
                f"  cpu_trace_stack_op(cpu, 0, {op_id}, _old_s, -2); }}",
            ]
        return [
            "{ uint16 _old_s = cpu->S;",
            "  if (cpu->x_flag) {",
            *(f"    {s}" for s in emitter_helpers.push_byte(widths.low_byte(field))),
            f"    cpu_trace_stack_op(cpu, 0, {op_id}, _old_s, -1);",
            "  } else {",
            *(f"    {s}" for s in emitter_helpers.push_word(field)),
            f"    cpu_trace_stack_op(cpu, 0, {op_id}, _old_s, -2);",
            "  } }",
        ]
    return [f"/* TODO PushReg({op.reg}) */"]


def _emit_pullreg(op: PullReg) -> List[str]:
    field = _reg(op.reg)
    if op.reg == Reg.P:
        return ["{ uint8 _old_p = cpu->P; uint16 _old_s = cpu->S;",
                *(f"  {s}" for s in emitter_helpers.pop_byte_assign(field)),
                "  cpu_p_to_mirrors(cpu);",
                "  cpu_trace_stack_op(cpu, 0, CPU_STACK_OP_PLP, _old_s, +1);",
                "  cpu_trace_event(cpu, 0, CPU_TR_PLP, _old_p, cpu->P);",
                "  cpu_trace_px_record(cpu, 0, 2 /*PLP*/, _old_p, cpu->P); }"]
    if op.reg == Reg.DB:
        # PLB sets N/Z from popped value.
        trace_pc = _trace_pc_arg()
        return ["{ uint8 _old_db = cpu->DB; uint16 _old_s = cpu->S;",
                *(f"  {s}" for s in emitter_helpers.pop_byte_assign(field)),
                *(f"  {s}" for s in widths.set_nz(field, 1)),
                "  cpu_trace_stack_op(cpu, 0, CPU_STACK_OP_PLB, _old_s, +1);",
                f"  cpu_trace_db_change(cpu, {trace_pc}, _old_db, cpu->DB, CPU_TR_PLB); }}"]
    if op.reg == Reg.PB:
        # PLK doesn't exist on the 65816 but IR routes any PullReg(PB) here.
        trace_pc = _trace_pc_arg()
        return ["{ uint8 _old_pb = cpu->PB; uint16 _old_s = cpu->S;",
                *(f"  {s}" for s in emitter_helpers.pop_byte_assign(field)),
                *(f"  {s}" for s in widths.set_nz(field, 1)),
                "  cpu_trace_stack_op(cpu, 0, CPU_STACK_OP_PLB, _old_s, +1);",
                f"  cpu_trace_pb_change(cpu, {trace_pc}, _old_pb, cpu->PB, CPU_TR_PB_WRITE); }}"]
    if op.reg == Reg.D:
        # PLD: 16-bit, sets N/Z from popped 16-bit value.
        return emitter_helpers.stack_op_traced(
            "CPU_STACK_OP_PLD", +2,
            emitter_helpers.pop_word_assign(field) + widths.set_nz(field, 2))
    # Final cpu->P sync line — both A and X/Y end with the same packed-flag update.
    p_sync = ("cpu->P = (uint8)((cpu->P & ~0x82) | "
              "(cpu->_flag_Z ? 0x02 : 0) | (cpu->_flag_N ? 0x80 : 0));")
    if op.reg == Reg.A:
        # PLA: width follows M. Preserve B (high byte) in m=1.
        # Prefer decoder static `op.static_m` over runtime cpu->m_flag
        # for the same reason as PushReg — keeps PHA/PLA brackets
        # balanced against caller-side flag drift.
        if op.static_m == 1:
            return [
                "{ uint16 _old_s = cpu->S;",
                *(f"  {s}" for s in emitter_helpers.pop_byte_assign("uint8 _v")),
                f"  {field} = {widths.preserve_high(field, '_v')};",
                *(f"  {s}" for s in widths.set_nz_no_p("_v", 1)),
                "  cpu_trace_stack_op(cpu, 0, CPU_STACK_OP_PLA, _old_s, +1);",
                f"  {p_sync} }}",
            ]
        if op.static_m == 0:
            return [
                "{ uint16 _old_s = cpu->S;",
                *(f"  {s}" for s in emitter_helpers.pop_word_assign(field)),
                *(f"  {s}" for s in widths.set_nz_no_p(field, 2)),
                "  cpu_trace_stack_op(cpu, 0, CPU_STACK_OP_PLA, _old_s, +2);",
                f"  {p_sync} }}",
            ]
        lines = ["{ uint16 _old_s = cpu->S;",
                 "  if (cpu->m_flag) {",
                 *(f"    {s}" for s in emitter_helpers.pop_byte_assign("uint8 _v")),
                 f"    {field} = {widths.preserve_high(field, '_v')};",
                 *(f"    {s}" for s in widths.set_nz_no_p("_v", 1)),
                 "    cpu_trace_stack_op(cpu, 0, CPU_STACK_OP_PLA, _old_s, +1);",
                 "  } else {",
                 *(f"    {s}" for s in emitter_helpers.pop_word_assign(field)),
                 *(f"    {s}" for s in widths.set_nz_no_p(field, 2)),
                 "    cpu_trace_stack_op(cpu, 0, CPU_STACK_OP_PLA, _old_s, +2);",
                 "  }",
                 f"  {p_sync} }}"]
        return lines
    if op.reg in (Reg.X, Reg.Y):
        op_id = "CPU_STACK_OP_PLX" if op.reg == Reg.X else "CPU_STACK_OP_PLY"
        # PLX/PLY: x=1 zero-extends (hw contract). Prefer decoder static
        # `op.static_x` over runtime cpu->x_flag for the same reason as
        # PushReg — keeps PH?/PL? brackets balanced when ROM bodies
        # bracket them with internal REP/SEP idioms.
        if op.static_x == 1:
            return [
                "{ uint16 _old_s = cpu->S;",
                *(f"  {s}" for s in emitter_helpers.pop_byte_assign("uint8 _v")),
                f"  {field} = {widths.zero_extend_lo('_v')};"
                f"  /* x=1 zeros high byte (hw contract) */",
                *(f"  {s}" for s in widths.set_nz_no_p("_v", 1)),
                f"  cpu_trace_stack_op(cpu, 0, {op_id}, _old_s, +1);",
                f"  {p_sync} }}",
            ]
        if op.static_x == 0:
            return [
                "{ uint16 _old_s = cpu->S;",
                *(f"  {s}" for s in emitter_helpers.pop_word_assign(field)),
                *(f"  {s}" for s in widths.set_nz_no_p(field, 2)),
                f"  cpu_trace_stack_op(cpu, 0, {op_id}, _old_s, +2);",
                f"  {p_sync} }}",
            ]
        lines = ["{ uint16 _old_s = cpu->S;",
                 "  if (cpu->x_flag) {",
                 *(f"    {s}" for s in emitter_helpers.pop_byte_assign("uint8 _v")),
                 f"    {field} = {widths.zero_extend_lo('_v')};"
                 f"  /* x=1 zeros high byte (hw contract) */",
                 *(f"    {s}" for s in widths.set_nz_no_p("_v", 1)),
                 f"    cpu_trace_stack_op(cpu, 0, {op_id}, _old_s, +1);",
                 "  } else {",
                 *(f"    {s}" for s in emitter_helpers.pop_word_assign(field)),
                 *(f"    {s}" for s in widths.set_nz_no_p(field, 2)),
                 f"    cpu_trace_stack_op(cpu, 0, {op_id}, _old_s, +2);",
                 "  }",
                 f"  {p_sync} }}"]
        return lines
    return [f"/* TODO PullReg({op.reg}) */"]


def _emit_transfer(op: Transfer) -> List[str]:
    """65816 register-transfer with width-respecting destination AND
    N/Z flag update on the transferred value. TXS / TCS DON'T set
    flags; TCS specifically is a 16-bit copy without flag update.
    Everything else does."""
    src = _reg(op.src)
    dst = _reg(op.dst)
    # TXS, TCS: no flag update, no width check (S is always 16-bit native).
    # TXS only transfers low byte of X (S high stays); TCS transfers all 16
    # bits. v1 emit didn't distinguish. Trace S changes for hunt-the-bug.
    if op.dst == Reg.S:
        return [
            "{ uint16 _old_s = cpu->S;",
            f"  {dst} = {src};",
            "  /* trace_event uses extra0/extra1 for old/new S high bytes */",
            "  cpu_trace_event(cpu, 0, CPU_TR_DB_WRITE,",
            "                  (uint8)(_old_s >> 8), cpu->S); }",
        ]
    # TDC (D->A) / TSC (S->A): the C-register transfers are ALWAYS 16-bit,
    # independent of the M flag (unlike TXA/TYA). Both set N/Z on the 16-bit
    # value. (TCD/TCS, the A->D/S direction, already go 16-bit via dst==D/S.)
    if op.dst == Reg.A and op.src in (Reg.D, Reg.S):
        return [f"{dst} = (uint16)({src});"] + widths.set_nz(dst, 2)
    # Determine destination width from controlling flag.
    if op.dst == Reg.A:
        flag = "cpu->m_flag"
    elif op.dst in (Reg.X, Reg.Y):
        flag = "cpu->x_flag"
    elif op.dst == Reg.D or op.dst == Reg.S:
        flag = None  # always 16-bit
    else:
        flag = None
    if flag is None:
        # Full-width transfer (D, etc.)
        return [f"{dst} = {src};"] + widths.set_nz(dst, 2)
    # X/Y dest in x=1 zero-extends (high byte hardware-zero); A dest in
    # m=1 preserves high byte (= B register). See _emit_writereg comment
    # for the LoadStripeImage failure that motivated this. 2026-04-30.
    if op.dst in (Reg.X, Reg.Y):
        dst_8bit = f"{dst} = {widths.zero_extend_lo('_v')};  /* x=1 zeros high byte (hw contract) */"
    else:
        dst_8bit = f"{dst} = {widths.preserve_high(dst, '_v')};"
    lines = [f"if ({flag}) {{",
             f"  uint8 _v = {widths.low_byte(src)};",
             f"  {dst_8bit}"]
    lines.extend(f"  {s}" for s in widths.set_nz_no_p("_v", 1))
    lines.append("} else {")
    lines.append(f"  {dst} = (uint16)({src});")
    lines.extend(f"  {s}" for s in widths.set_nz_no_p(dst, 2))
    lines.append("}")
    # Final cpu->P sync after both branches.
    lines.append("cpu->P = (uint8)((cpu->P & ~0x82) | "
                 "(cpu->_flag_Z ? 0x02 : 0) | (cpu->_flag_N ? 0x80 : 0));")
    return lines


def _emit_condbranch(op: CondBranch) -> List[str]:
    pred = f"{_reg(op.flag)} == {op.take_if}"
    # The actual goto target is encoded by the caller (block-level emit) since
    # the IR op itself doesn't store the target — the cfg edge does.
    return [f"if ({pred}) {{ /* take branch — caller fills label */ }}"]


def _emit_goto(op: Goto) -> List[str]:
    # Caller (block-level emit) fills the goto target.
    return ["/* Goto — caller fills label */"]


def _emit_indirect_goto(op: IndirectGoto) -> List[str]:
    """Standalone IndirectGoto codegen (no dispatch_entries resolved).

    Used to emit a `/* IndirectGoto: ... */` stub when the decoder
    couldn't resolve targets — but the no-stub policy means this stub
    must never reach src/gen/. Callers in emit_function.py route
    dispatch_entries-resolved sites through `_emit_indirect_dispatch`
    instead, and v2_regen hard-fails on any unresolved site BEFORE
    src/gen/ is consumed. This emit stays as a defensive guard so the
    function signature is preserved.
    """
    bank, addr = _segref_addr_expr(op.seg)
    return [f"/* IndirectGoto: target = ({bank}, {addr}) — caller dispatches */"]


def _emit_indirect_dispatch(insn) -> List[str]:
    """Emit a real switch for an indirect JMP/JML/JSR whose static
    target list the decoder recovered (via cfg `indirect_dispatch` or
    auto-recovery). Switches on the index register declared by the
    cfg directive (X or Y); each case tail-calls the corresponding
    handler.

    The dispatching JMP/JML is a TERMINATOR (control transfers to the
    handler; the handler's own RTL/RTS returns to the dispatcher's
    caller, not back into the dispatcher). So each case emits a `return
    <handler>(cpu);` rather than a fall-through-after-call. This
    matches the asm idiom where `JSR Module_MainRouting` pushes a 16-bit
    return; Module_MainRouting's `JML [DP]` transfers to the handler;
    the handler `RTL`s and pops PB+PC — meaning the handler must have
    been entered via JSL (or the dispatch's `JML` effectively converts
    a 24-bit-target call into a tail-call from the JSL caller's POV).

    Empty `case 0:` ⇒ null entry (table padding); emits `default: break`-
    style fall-through that returns NORMAL.

    OOB index ⇒ runtime trap. No silent stub.
    """
    bank = (insn.addr >> 16) & 0xFF
    entries = insn.dispatch_entries
    idx_reg = getattr(insn, 'dispatch_idx_reg', 'X')
    if idx_reg not in ('X', 'Y'):
        idx_reg = 'X'
    n = len(entries)
    site_pc24 = insn.addr & 0xFFFFFF
    kind = getattr(insn, 'dispatch_kind', 'short')
    # JSR (abs,X) and JMP (abs,X) reach this same emitter, but their
    # post-dispatch control flow differs: JMP is terminal (handler's
    # RTS pops the JMP-CALLER's return, not the dispatcher's), so each
    # case can `return` straight through. JSR is non-terminal — its
    # own pushed return address sits below the handler's RTS, so the
    # handler returns to the dispatcher and execution must fall
    # through to the next block. emit_function.py mirrors this by
    # leaving `block_terminated = False` for JSR, expecting the switch
    # to break out and the post-call block trace to emit. So:
    # JSR → emit `break;`, fall through to next block.
    # JMP/JML → emit `return RECOMP_RETURN_NORMAL;` (terminal).
    is_jsr = getattr(insn, 'mnem', '') == 'JSR'
    # Pointer-sourced CALL idiom (`PEA <ret>; JMP (ptr)`): a JMP that is NOT a
    # tail transfer — the PEA already pushed the real return frame, the
    # dispatched handler RTSes back to it, and execution resumes the next
    # sequential block. So it behaves like a JSR call (fall through, handler
    # host-returns) but WITHOUT synthesizing its own return frame.
    is_call = bool(getattr(insn, 'dispatch_call', False))
    is_pointer_match = bool(
        getattr(insn, 'dispatch_pointer_match', False))
    # Variant suffix for dispatched handlers follows the live width state
    # at the dispatch site. The PHA/SEP/RTS idiom is the exception: it
    # explicitly forces M/X to 8-bit before the synthetic RTS transfer.
    is_rts_stack_dispatch = bool(getattr(insn, 'dispatch_terminal', False))
    if is_rts_stack_dispatch:
        em = getattr(insn, 'dispatch_forced_m', 1) & 1
        ex = getattr(insn, 'dispatch_forced_x', 1) & 1
    else:
        em = getattr(insn, 'm_flag', 1) & 1
        ex = getattr(insn, 'x_flag', 1) & 1
    suffix = _variant_suffix(em, ex)

    # Comment marker differentiates JSR (call, fall-through) from
    # JMP/JML (terminator, tail-call) for downstream tooling and
    # regression tests.
    if is_rts_stack_dispatch:
        _comment = "RTS-stack dispatch terminator: cfg-resolved target list"
    elif is_jsr and is_call and is_pointer_match:
        _comment = "indirect dispatch pointer-call (JSR (abs,X)): cfg-resolved target list"
    elif is_jsr and is_call:
        _comment = "indirect dispatch call: cfg-resolved target list"
    elif is_call:
        _comment = ("indirect dispatch ptr-call "
                    f"({'PHK+PEA+JML' if kind == 'long' else 'PEA+JMP'} idiom): "
                    "cfg-resolved target list")
    else:
        _comment = ("indirect dispatch call: cfg-resolved target list" if is_jsr
                    else "indirect dispatch terminator: cfg-resolved target list")
    lines = [f"{{ /* {_comment} */"]
    # Option-1 cpu->S ABI: a real JSR (abs,X) call pushes a 2-byte return
    # frame and enters the handler with host_return_valid=2 (the frame
    # size — see cpu_state.h). A JMP/JML indirect dispatch is a tail
    # transfer — no frame pushed; the handler
    # inherits THIS function's host-return validity (_hrv). The handler also
    # inherits THIS function's entry-S baseline at the individual call site,
    # so split shared suffixes do not record a fresh stack baseline after the
    # tail transfer.
    if is_jsr:
        _iret16 = (site_pc24 + 2) & 0xFFFF  # JSR (abs,X) is 3 bytes; push return-1
        lines.append(f"  cpu_write8(cpu, 0x00, cpu->S, 0x{(_iret16 >> 8) & 0xFF:02x}); cpu->S = (uint16)(cpu->S - 1);")
        lines.append(f"  cpu_write8(cpu, 0x00, cpu->S, 0x{_iret16 & 0xFF:02x}); cpu->S = (uint16)(cpu->S - 1);")
        lines.append("  cpu->host_return_valid = 2;  /* indirect JSR call, 2-byte frame */")
    elif is_call:
        # PEA already pushed the return frame; enter the handler as a paired
        # host call so its balanced RTS host-returns here (and pops the PEA'd
        # frame), then we fall through to the next block. The long form has
        # an immediately preceding PHK byte which the handler's RTL consumes
        # along with PEA's two-byte PC.
        call_frame_size = 3 if kind == 'long' else 2
        lines.append(
            f"  cpu->host_return_valid = {call_frame_size};  "
            f"/* {'PHK+PEA+JML' if kind == 'long' else 'PEA+JMP'} indirect call, "
            f"{call_frame_size}-byte frame */")
    else:
        lines.append("  cpu->host_return_valid = _hrv;  /* JMP/JML indirect tail dispatch */")
    # Index source: X or Y register. For JMP/JSR (abs,X)-style dispatch
    # (table_bases empty — the dispatch consumes the operand directly as
    # `(table, X)`), the asm shifts the entry index up by the entry size
    # BEFORE the TAX: 16-bit pointer table = `ASL A; TAX` → X is a byte
    # offset into the table; 24-bit pointer table = `ASL; ASL; ADC; TAX`
    # → X is a 3-byte offset. The switch needs the LOGICAL entry index,
    # so divide the register by the entry size before switching. The DP-
    # built-pointer form (table_bases non-empty: parallel byte tables
    # indexed directly) doesn't need the divide — the asm uses the index
    # register as-is to load one byte per parallel table.
    idx_field = 'X' if idx_reg == 'X' else 'Y'
    entry_size = 3 if kind == 'long' else 2
    table_bases = tuple(getattr(insn, 'dispatch_table_bases', ()) or ())
    if getattr(insn, 'dispatch_local_goto', False):
        ptr = insn.operand & 0xFFFF
        lines = [
            "{ /* local computed-goto dispatch: recovered same-function runway */",
        ]
        if getattr(insn, 'dispatch_stack_pointer', False):
            lines.append(
                f"  uint16 _target = (uint16)(cpu_read16(cpu, 0x00, "
                f"(uint16)(cpu->D + 0x{ptr:04x})) + 1u);"
                "  /* PEI(dp); RTS consumes target-1 as an internal goto */")
        else:
            lines.append(
                f"  uint16 _target = cpu_read16(cpu, cpu->PB, (uint16)0x{ptr:04x});"
                "  /* absolute indirect dispatch: switch on the loaded pointer */")
        lines.append("  switch (_target) {")
        seen_cases = set()
        for e in entries:
            if e is None or e == 0:
                continue
            target_bank = (e >> 16) & 0xFF
            if target_bank != bank:
                continue
            local_pc = e & 0xFFFF
            if local_pc in seen_cases:
                continue
            seen_cases.add(local_pc)
            lines.append(
                f"    case 0x{local_pc:04x}: goto L_{local_pc:04X}{suffix};")
        lines.append("    default:")
        lines.append(
            f"      (void)cpu_trace_dispatch_oob(cpu, 0x{site_pc24:06x}, _target);")
        # Interpreter-fallback tier: run the loaded target instead of dropping
        # it (interp shares CpuState + the AOT bus; bail -> abandon-balanced
        # fallback, never worse than the drop). See docs/MULTI_TIER.md.
        lines.append(
            f"      return interp_tier_dispatch_tail(cpu, "
            f"((uint32)cpu->PB << 16) | _target, "
            f"0x{site_pc24:06x}u, _entry_s, _hrv);")
        lines.append("  }")
        lines.append("}")
        return lines
    value_pointer_dispatch = False
    pointer_is_indexed = False
    if getattr(insn, 'mode', None) == INDIR and len(table_bases) == 1:
        value_pointer_dispatch = True
    elif ((is_pointer_match
           or (is_jsr and is_call))
          and getattr(insn, 'mode', None) == INDIR_X
          and len(table_bases) == 1):
        value_pointer_dispatch = True
        pointer_is_indexed = True
    if value_pointer_dispatch:
        ptr = insn.operand & 0xFFFF
        if kind == 'long' and pointer_is_indexed:
            lines.append(
                f"  uint16 _ptr = (uint16)(0x{ptr:04x} + (cpu->{idx_field} & 0xFFFF));"
                "  /* long pointer descriptor selected at runtime */")
            lines.append(
                "  uint16 _target_lo = cpu_read16(cpu, 0x00, _ptr);")
            lines.append(
                "  uint8 _target_bank = cpu_read8(cpu, 0x00, (uint16)(_ptr + 2));")
            lines.append(
                "  uint32 _target = ((uint32)_target_bank << 16) | (uint32)_target_lo;"
                "  /* switch on the loaded long pointer */")
        elif kind == 'long':
            lines.append(
                f"  uint16 _target_lo = cpu_read16(cpu, 0x00, (uint16)0x{ptr:04x});")
            lines.append(
                f"  uint8 _target_bank = cpu_read8(cpu, 0x00, (uint16)(0x{ptr:04x} + 2));")
            lines.append(
                "  uint32 _target = ((uint32)_target_bank << 16) | (uint32)_target_lo;"
                "  /* absolute long-indirect dispatch: switch on the loaded pointer */"
            )
        elif pointer_is_indexed:
            lines.append(
                f"  uint16 _ptr = (uint16)(0x{ptr:04x} + (cpu->{idx_field} & 0xFFFF));"
                "  /* JSR (abs,X): X selects a runtime pointer descriptor */")
            lines.append(
                "  uint16 _target = cpu_read16(cpu, cpu->PB, _ptr);"
                "  /* switch on the loaded pointer */")
        else:
            lines.append(
                f"  uint16 _target = cpu_read16(cpu, cpu->PB, (uint16)0x{ptr:04x});"
                "  /* absolute indirect dispatch: switch on the loaded pointer */"
            )
        if is_rts_stack_dispatch:
            lines.append("  {")
            for stmt in emitter_helpers.modify_p_via_mirrors(0x30, "sep"):
                lines.append(f"    {stmt}")
            lines.append("  }")
        lines.append("  switch (_target) {")
        # Multiple table slots may resolve to the same target PC (e.g., a
        # dispatch table with shared/default handlers). The switch keys
        # on the loaded pointer, so emit one case per unique target PC —
        # duplicate case labels are a hard C error (zelda_01 hit this on
        # repeated 0x8A92 / 0x8B0D entries).
        seen_cases = set()
        for i, e in enumerate(entries):
            if e is None or e == 0:
                continue
            target_bank = (e >> 16) & 0xFF
            local_pc = e & 0xFFFF
            tgt_addr = e & 0xFFFFFF
            case_value = tgt_addr if kind == 'long' else local_pc
            if case_value in seen_cases:
                continue
            seen_cases.add(case_value)
            base_name = _NAME_RESOLVER.get(tgt_addr)
            if base_name is None:
                base_name = f"bank_{target_bank:02X}_{local_pc:04X}"
            case_label = f"0x{case_value:06x}" if kind == 'long' else f"0x{case_value:04x}"
            lines.append(f"    case {case_label}: {{")
            if is_rts_stack_dispatch:
                # SEP #$30 forced m=x=1 immediately above; the M1X1
                # architectural variant is the only one ever entered. It is
                # still exact: a manifest NULL slot executes LLE rather than
                # creating an undefined reference or borrowing a sibling.
                if has_exact_variant(tgt_addr, em, ex):
                    _UNRESOLVED_CALL_TARGETS.add((tgt_addr, em, ex))
                    name = f"{base_name}{suffix}"
                    env = emitter_helpers.call_with_pb_save(
                        target_bank, name, trace_pc24=site_pc24)
                    for stmt in env:
                        lines.append(f"      {stmt}")
                else:
                    lines.append(
                        f"      return interp_tier_dispatch_tail(cpu, "
                        f"0x{tgt_addr:06x}u, 0x{site_pc24:06x}u, "
                        f"_entry_s, _hrv); /* authoritative LLE M1X1 */")
            else:
                # General indirect dispatch: runtime (m, x) dispatch
                # inside one PB save/restore envelope so the wrong-
                # variant class is eliminated for indirect calls too.
                for em_v, ex_v in valid_variant_list(tgt_addr):
                    _UNRESOLVED_CALL_TARGETS.add((tgt_addr, em_v, ex_v))
                body = [
                    "RecompReturn _r;",
                    "switch (((cpu->m_flag & 1) << 1) | (cpu->x_flag & 1)) {",
                ]
                # A pointer-matched JMP/JML is still a true tail transfer.
                # Its selected handler inherits the dispatcher's architectural
                # return frame/baseline; entering it as an ordinary C call
                # silently re-bases _entry_s at the current S.  That is often
                # numerically equal until a shared suffix consumes an outer
                # JSL frame (DKC2: kong state -> sprite_return_address), where
                # the missing ownership contract turns the final RTL into a
                # fresh dispatch and corrupts PB/S.
                # A ptrtail_popcall helper has physically PLA'd its incoming
                # JSR frame, but its prologue already inherited the enclosing
                # caller's logical return context from the terminal JSR site.
                # Hand that unchanged context to the selected state target.
                # Deriving a new baseline from the post-PLA S leaks or consumes
                # one outer frame per state transition.
                _pre = (["cpu_tailcall_inherit_return_context(_entry_s, _hrv);"]
                        if not (is_jsr or is_call) else None)
                body += variant_dispatch_case_lines(
                    tgt_addr, base_name, indent="  ", pre_call=_pre,
                    lle_fallback=(
                        f"interp_tier_run_call_frame(cpu, 0x{tgt_addr:06x}u, "
                        f"0x{site_pc24:06x}u, 2, NULL)"
                        if is_jsr or is_call else
                        f"interp_tier_dispatch_tail(cpu, 0x{tgt_addr:06x}u, "
                        f"0x{site_pc24:06x}u, _entry_s, _hrv)"))
                body.append("}")
                if is_jsr or is_call:
                    env = emitter_helpers.pb_save_restore_envelope(
                        target_bank, body, trace_pc24=site_pc24)
                    for stmt in env:
                        lines.append(f"      {stmt}")
                else:
                    # A JMP/JML dispatch hands off this function's return
                    # obligation. It is not a host call which later resumes
                    # here, so restoring the dispatcher's PB after the
                    # handler returns would overwrite the PB selected by the
                    # handler's architectural RTS/RTL. Short JMP preserves
                    # the live PB (including LoROM/HiROM mirrors); long JML
                    # changes it to the selected target bank.
                    if kind == 'long':
                        lines.append(
                            f"      cpu->PB = 0x{target_bank:02x};  "
                            "/* long indirect tail transfer */")
                    for stmt in body:
                        lines.append(f"      {stmt}")
                    lines.append("      return _r;")
            if is_jsr or is_call:
                lines.append("      break;")
            # True tail transfers returned _r directly above.
            lines.append("    }")
        if is_jsr or is_call:
            # Unknown runtime pointer on a CALL-form dispatch: account
            # the miss (was previously SILENT — no dispatch_oob call at
            # all), then unpop the unconsumed 2-byte call frame (the JSR
            # form pushed it above; the PEA+JMP form's PEA pushed it) so
            # the fall-through resumes balanced — as if the unreached
            # handler had returned immediately.
            lines.append("    default:")
            lines.append(
                f"      (void)cpu_trace_dispatch_oob(cpu, 0x{site_pc24:06x}, _target);")
            lines.append(
                "      cpu->S = (uint16)(cpu->S + 2);  /* unpop unconsumed call frame */")
            lines.append("      break;")
        else:
            lines.append("    default: break;")
        lines.append("  }")
        if is_jsr or is_call:
            lines.append("  /* fall through to post-dispatch block */")
        else:
            # Terminal tail dispatch with an unknown pointer: account the
            # miss, then abandon this invocation balanced (discard locals
            # below _entry_s, pop the paired caller's frame per _hrv).
            # The pre-2026-06-09 bare `return cpu_trace_dispatch_oob(...)`
            # orphaned the caller's frame on every hit (SM cinematic
            # sub-dispatcher leak -> WILD STACK crash).
            lines.append(
                f"  (void)cpu_trace_dispatch_oob(cpu, 0x{site_pc24:06x}, _target);")
            # Interpreter-fallback tier: run the loaded target instead of
            # dropping it; bail -> abandon-balanced fallback. docs/MULTI_TIER.md.
            fallback_target = (
                "_target" if kind == 'long'
                else "((uint32)cpu->PB << 16) | _target")
            lines.append(
                f"  return interp_tier_dispatch_tail(cpu, "
                f"{fallback_target}, "
                f"0x{site_pc24:06x}u, _entry_s, _hrv);")
        lines.append("}")
        return lines
    if len(table_bases) >= 2:
        lines.append(
            f"  uint16 _idx = (uint16)(cpu->{idx_field} & 0xFFFF);"
            "  /* parallel byte tables: register already holds logical index */"
        )
    else:
        lines.append(
            f"  uint16 _idx = (uint16)((cpu->{idx_field} & 0xFFFF) / {entry_size});"
            f"  /* entry_size={entry_size} ({kind}); ASL[*N] + TAX in asm => "
            f"{idx_field} is byte offset, divide back to logical index */"
        )
    if is_rts_stack_dispatch:
        lines.append("  {")
        for stmt in emitter_helpers.modify_p_via_mirrors(0x30, "sep"):
            lines.append(f"    {stmt}")
        lines.append("  }")
    lines.append(f"  static const uint16 _disp_n = {n};")
    lines.append("  if (_idx >= _disp_n) {")
    if is_jsr or is_call:
        # Non-terminal CALL-form dispatch: account the OOB index, then
        # unpop the unconsumed 2-byte call frame (the JSR form pushed it
        # above; the PEA+JMP form's PEA pushed it) and fall through to
        # the post-dispatch block balanced — as if the unreached handler
        # had returned immediately. (The switch below falls out via its
        # default arm.)
        lines.append(
            f"    (void)cpu_trace_dispatch_oob(cpu, 0x{site_pc24:06x}, _idx);")
        lines.append(
            "    cpu->S = (uint16)(cpu->S + 2);  /* unpop unconsumed call frame */")
    else:
        # Terminal tail dispatch with an OOB index: account the miss,
        # then abandon this invocation balanced (discard locals below
        # _entry_s, pop the paired caller's frame per _hrv). The bare
        # `return cpu_trace_dispatch_oob(...)` this replaces orphaned
        # the caller's frame on every hit.
        lines.append(
            f"    (void)cpu_trace_dispatch_oob(cpu, 0x{site_pc24:06x}, _idx);")
        lines.append(
            f"    return cpu_unresolved_abandon_balanced(cpu, "
            f"0x{site_pc24:06x}u, _entry_s, _hrv);")
    lines.append("  }")
    lines.append("  switch (_idx) {")
    for i, e in enumerate(entries):
        if e is None or e == 0:
            if is_jsr or is_call:
                # Null entry on a CALL-form dispatch: no handler runs, so
                # the call frame pushed above is never consumed — unpop it
                # before falling through, same as the OOB arm. (A hit is
                # still suspicious — real hw would JSR into $0000 — so
                # account it like an OOB index.)
                lines.append(f"    case {i}:")
                lines.append(
                    f"      (void)cpu_trace_dispatch_oob(cpu, 0x{site_pc24:06x}, _idx);")
                lines.append(
                    "      cpu->S = (uint16)(cpu->S + 2);  /* unpop unconsumed call frame */")
                lines.append("      break; /* null entry */")
            else:
                # Null entry on a terminal tail dispatch: the bare
                # `return RECOMP_RETURN_NORMAL` this replaces orphaned
                # the caller's frame exactly like the OOB arm.
                lines.append(f"    case {i}:")
                lines.append(
                    f"      (void)cpu_trace_dispatch_oob(cpu, 0x{site_pc24:06x}, _idx);")
                lines.append(
                    f"      return cpu_unresolved_abandon_balanced(cpu, "
                    f"0x{site_pc24:06x}u, _entry_s, _hrv); /* null entry */")
            continue
        target_bank = (e >> 16) & 0xFF
        local_pc = e & 0xFFFF
        tgt_addr = e & 0xFFFFFF
        base_name = _NAME_RESOLVER.get(tgt_addr)
        if base_name is None:
            base_name = f"bank_{target_bank:02X}_{local_pc:04X}"
        # JMP/JML path: dispatched handler's return value propagates
        # straight back to OUR caller (tail-call semantics — `return _r`).
        # JSR path: handler returns to dispatcher; SKIP-N propagation
        # still goes back through the caller chain via the standard
        # NLR-propagation check, but on NORMAL the case breaks out of
        # the switch so the post-JSR block continues. PB save/restore
        # happens around the call so PHK inside the handler pushes
        # the target bank.
        lines.append(f"    case {i}: {{")
        if is_rts_stack_dispatch:
            # SEP #$30 forced m=x=1 immediately above; single-variant
            # _M1X1 is the only one ever entered, but it may intentionally
            # remain LLE in the authoritative manifest.
            if has_exact_variant(tgt_addr, em, ex):
                _UNRESOLVED_CALL_TARGETS.add((tgt_addr, em, ex))
                name = f"{base_name}{suffix}"
                env = emitter_helpers.call_with_pb_save(
                    target_bank, name, trace_pc24=site_pc24)
                for stmt in env:
                    if (not (is_jsr or is_call)
                            and stmt.endswith(f"{name}(cpu);")):
                        lines.append(
                            "      cpu_tailcall_inherit_return_context(_entry_s, _hrv);")
                    lines.append(f"      {stmt}")
            else:
                lines.append(
                    f"      return interp_tier_dispatch_tail(cpu, "
                    f"0x{tgt_addr:06x}u, 0x{site_pc24:06x}u, "
                    f"_entry_s, _hrv); /* authoritative LLE M1X1 */")
        else:
            # General indirect dispatch: runtime (m, x) dispatch inside
            # one PB save/restore envelope. Eliminates the wrong-variant
            # class for indirect calls (Dr. Light freeze 2026-05-23
            # traced into a CC84_M1X1 entered at runtime M1X0 via this
            # path).
            for em_v, ex_v in valid_variant_list(tgt_addr):
                _UNRESOLVED_CALL_TARGETS.add((tgt_addr, em_v, ex_v))
            body = [
                "RecompReturn _r;",
                "switch (((cpu->m_flag & 1) << 1) | (cpu->x_flag & 1)) {",
            ]
            # Tail-context inherit is for TRUE tail transfers only. A JSR
            # dispatch and a PEA+JMP ptr-call both enter the handler as a
            # paired host call (fresh frame on cpu->S, hrv=2 set above) —
            # inheriting OUR baseline there would make the handler's RTS
            # misclassify its own balanced return.
            _pre = (["cpu_tailcall_inherit_return_context(_entry_s, _hrv);"]
                    if not (is_jsr or is_call) else None)
            body += variant_dispatch_case_lines(
                tgt_addr, base_name, indent="  ", pre_call=_pre,
                lle_fallback=(
                    f"interp_tier_run_call_frame(cpu, 0x{tgt_addr:06x}u, "
                    f"0x{site_pc24:06x}u, 2, NULL)"
                    if is_jsr or is_call else
                    f"interp_tier_dispatch_tail(cpu, 0x{tgt_addr:06x}u, "
                    f"0x{site_pc24:06x}u, _entry_s, _hrv)"))
            body.append("}")
            if is_jsr or is_call:
                env = emitter_helpers.pb_save_restore_envelope(
                    target_bank, body, trace_pc24=site_pc24)
                for stmt in env:
                    lines.append(f"      {stmt}")
            else:
                # True tail dispatch: preserve the live PB for short JMP;
                # install the selected bank for long JML; never restore the
                # dispatcher's PB after the handler consumes our caller's
                # architectural return frame.
                if kind == 'long':
                    lines.append(
                        f"      cpu->PB = 0x{target_bank:02x};  "
                        "/* long indirect tail transfer */")
                for stmt in body:
                    lines.append(f"      {stmt}")
                lines.append("      return _r;")
        if is_jsr or is_call:
            # Handler host-returned (it popped the call frame). Fall
            # through to the post-dispatch block. (is_call previously
            # emitted a terminal `return NORMAL` here, inconsistent with
            # the Form-A switch-on-pointer emit AND with emit_function's
            # `block_terminated = not dispatch_call` — it silently
            # dropped the post-JMP code the PEA'd return targets.)
            lines.append("      break;")
        # True tail transfers returned _r directly above.
        lines.append("    }")
    lines.append("    default: break; /* unreachable: gated above */")
    lines.append("  }")
    if is_jsr or is_call:
        # Switch ended; fall through into the next block emitted after
        # this dispatcher (the post-JSR / post-ptr-call block in the
        # original asm).
        lines.append("  /* fall through to post-JSR block */")
    else:
        # Unreachable (OOB gated above), kept as a defensive backstop —
        # balanced like the gate arm.
        lines.append(
            f"  (void)cpu_trace_dispatch_oob(cpu, 0x{site_pc24:06x}, _idx);")
        lines.append(
            f"  return cpu_unresolved_abandon_balanced(cpu, "
            f"0x{site_pc24:06x}u, _entry_s, _hrv);")
    lines.append("}")
    return lines


def _emit_runtime_dispatch(insn) -> List[str]:
    """Emit a true RUNTIME-pointer indirect dispatch for a reachable
    JSR (abs,X) whose pointer-table base lives in WRAM ($0000-$1FFF) and
    whose target is written at run time (per-object handler pointer — SM's
    enemy/PLM/eproj instruction-list interpreters dispatching
    `JSR ($0FA8/$0FAE/$0FB0/$0FB2,X)` in banks $22-$2A). The decoder marks
    these `dispatch_runtime` (see decoder.py runtime-pointer recovery); they
    have NO statically-enumerable target list, so there is no switch.

    Reads the 16-bit pointer from PB:(base + idx) at run time and hands
    (PB<<16 | ptr) to cpu_dispatch_call_pc, which pushes the 2-byte JSR
    return frame, looks up the live (m,x) variant, and either calls the AOT
    body (paired host-call, host-returns through the frame) or runs the
    target on the interpreter tier. Either way the stack is balanced and
    execution falls through to the post-JSR block. Every dispatch is logged
    in the always-on g_dispatch_log ring. A non-NORMAL return is an NLR
    (return-to-ancestor) and is propagated like any other call site.
    """
    site_pc24 = insn.addr & 0xFFFFFF
    base = insn.operand & 0xFFFF
    idx_reg = getattr(insn, 'dispatch_idx_reg', 'X')
    idx_field = 'Y' if idx_reg == 'Y' else 'X'
    return [
        f"{{ /* runtime indirect dispatch JSR (${base:04X},{idx_field}): "
        f"per-object WRAM function pointer, resolved + dispatched at run time */",
        f"  uint16 _disp_ptr = cpu_read16(cpu, cpu->PB, "
        f"(uint16)(0x{base:04x}u + (uint16)(cpu->{idx_field} & 0xFFFFu)));",
        f"  RecompReturn _disp_r = cpu_dispatch_call_pc(cpu, "
        f"((uint32)cpu->PB << 16) | (uint32)_disp_ptr, 0x{site_pc24:06x}u);",
        "  if (_disp_r != RECOMP_RETURN_NORMAL) {",
        "    cpu_trace_event(cpu, 0, CPU_TR_NLR_PROPAGATE, (uint8)_disp_r, 0);",
        "    cpu_trace_mark_nlr_exit(BD_EXIT_KIND_SKIP_PROPAGATION);",
        "    return (RecompReturn)((int)_disp_r - 1);",
        "  }",
        "  /* fall through to post-JSR block */",
        "}",
    ]


def _emit_dispatch(insn) -> List[str]:
    """Emit a return-address jump-table dispatch as a static function-pointer
    array indexed by A. The 65816 dispatcher pops its return PC,
    indexes the table at that PC by A (×2 for short, ×3 for long),
    and JMPs through. Effective semantics: select handler by A then
    call. After return, this insn is a TERMINATOR (control returns
    to JSL's caller's caller, not to the bytes after this JSL).

    For each table entry:
      - non-zero, in this bank: emit handler call by friendly name
        (or synthetic bank_BB_AAAA), update PB save/restore, etc.
      - zero: emit a `default: break;` which becomes RTS-style return
    """
    bank = (insn.addr >> 16) & 0xFF
    entries = insn.dispatch_entries
    kind = getattr(insn, 'dispatch_kind', 'short')
    n = len(entries)
    is_short_call_helper = getattr(insn, 'mnem', '') == 'JSR'
    # The recomp bypasses the dispatch trampoline body (ExecutePtr /
    # ExecutePtrLong at $00:847 / $00:864 in SMW) and calls the handler
    # directly. The asm trampolines END with `SEP #$30` before JMLing
    # to the dispatched handler — by ROM contract, every handler is
    # entered with (m=1, x=1) regardless of caller-side runtime state.
    # Synthesize the same contract here:
    #  (1) Force the variant suffix to _M1X1 (matches what the handler
    #      sees on real hardware).
    #  (2) Emit a SEP-equivalent runtime reset of m_flag/x_flag/P so
    #      width-sensitive ops inside the handler observe (m=1, x=1)
    #      instead of inheriting the caller's (possibly drifted) flags.
    #
    # Iggy boss-platform freeze (2026-05-15) was rooted here:
    # CallSpriteMain's `JSL ExecutePtr` reached the IggyLarry handler
    # with runtime (m=1, x=0) inherited from caller, because the
    # synthesized dispatch skipped the trampoline's SEP. PHX/PLX
    # codegen (now static-width, see PushReg/PullReg) is one mitigation
    # but only fixes stack — it doesn't restore wrong X-width memory
    # reads further inside the handler. This reset closes that gap.
    # A short-call helper consumes its JSR return frame and tail-jumps while
    # preserving the caller's live widths. DKC2's sprite-state helpers use
    # this M0X0 form. Long-call ExecutePtr helpers retain their historical
    # SEP #$30 contract.
    em = (getattr(insn, 'm_flag', 1) & 1) if is_short_call_helper else 1
    ex = (getattr(insn, 'x_flag', 1) & 1) if is_short_call_helper else 1
    suffix = _variant_suffix(em, ex)
    site_pc24 = insn.addr & 0xFFFFFF
    lines = ["{ /* JSL dispatch — short=2B / long=3B table */"]
    if is_short_call_helper:
        lines[0] = lines[0].replace("JSL dispatch", "JSR dispatch")
    lines.append(f"  static const uint16 _disp_n = {n};")
    lines.append(f"  uint16 _idx = (uint16){widths.masked('cpu->A', 1)};")
    # OOB index: account the miss (previously a SILENT bare `return
    # NORMAL`), then abandon this invocation balanced. The dispatch is a
    # terminator — the matched handler's RTS pops the containing
    # function's caller's frame on its behalf, so a no-handler exit must
    # pop it too (cpu_unresolved_abandon_balanced) or the frame orphans.
    lines.append("  if (_idx >= _disp_n) {")
    lines.append(
        f"    (void)cpu_trace_dispatch_oob(cpu, 0x{site_pc24:06x}, _idx);")
    lines.append(
        f"    return cpu_unresolved_abandon_balanced(cpu, "
        f"0x{site_pc24:06x}u, _entry_s, _hrv); /* dispatch OOB */")
    lines.append("  }")
    if not is_short_call_helper:
        # Trampoline contract: dispatched handler observes (m=1, x=1).
        # Mirror the SEP #$30 the long-call helper performs.
        lines.append("  {")
        lines.append("    uint8 _old_p = cpu->P;")
        lines.append("    cpu_mirrors_to_p(cpu);")
        lines.append("    cpu->P = (uint8)(cpu->P | 0x30);")
        lines.append("    cpu_p_to_mirrors(cpu);")
        lines.append("    cpu_trace_px_record(cpu, 0, 1 /*SEP*/, _old_p, cpu->P);")
        lines.append("  }")
    # Option-1 cpu->S ABI: the ExecutePtr trampoline's own return frame is
    # discarded by the PLA*N IR ops preceding this dispatch (now emitted as
    # normal cpu->S pops). The dispatched handler is a dispatch-trampoline
    # target with no paired host-C caller -> enter with host_return_valid=0
    # so its RTS/RTL re-dispatches on the popped PC rather than host-return.
    lines.append("  cpu->host_return_valid = 0;  /* dispatch-trampoline target */")
    lines.append("  switch (_idx) {")
    for i, e in enumerate(entries):
        if e == 0:
            # Null entry: real hw would JMP through $0000 — a hit is a
            # bug worth accounting. The old `break` fell through to the
            # terminal `return NORMAL` WITHOUT popping the containing
            # function's caller's frame (the matched-handler path pops it
            # via the handler's RTS) — orphan leak. Abandon balanced.
            lines.append(f"    case {i}:")
            lines.append(
                f"      (void)cpu_trace_dispatch_oob(cpu, 0x{site_pc24:06x}, _idx);")
            lines.append(
                f"      return cpu_unresolved_abandon_balanced(cpu, "
                f"0x{site_pc24:06x}u, _entry_s, _hrv); /* null entry */")
            continue
        if kind == 'long':
            target_bank = (e >> 16) & 0xFF
            local_pc = e & 0xFFFF
            tgt_addr = e
        else:
            target_bank = bank
            local_pc = e & 0xFFFF
            tgt_addr = (bank << 16) | local_pc
        base_name = _NAME_RESOLVER.get(tgt_addr)
        if base_name is None:
            base_name = f"bank_{target_bank:02X}_{local_pc:04X}"
        # Multi-line case body: emit each statement on its own line so
        # the per-line scanner in emit_function.py can auto-inject
        # RecompStackPop() before any line starting with "return"
        # (the SKIP propagation block inside call_with_pb_save).
        # Single-line emit silently dropped the RecompStackPop on NLR
        # paths through the dispatcher — caught by GameMode oscillation
        # at boot 2026-05-02.
        lines.append(f"    case {i}: {{")
        if has_exact_variant(tgt_addr, em, ex):
            # The compact manifest is authoritative: reference an AOT symbol
            # only when the exact architectural entry (M1X1 after the
            # trampoline's SEP #$30) was emitted.
            _UNRESOLVED_CALL_TARGETS.add((tgt_addr, em, ex))
            name = f"{base_name}{suffix}"
            env = emitter_helpers.call_with_pb_save(
                target_bank, name, trace_pc24=site_pc24)
            for stmt in env:
                lines.append(f"      {stmt}")
        else:
            # ExecutePtr discarded its own return frame before tail-jumping
            # to the selected handler.  Interpret the missing exact body with
            # the enclosing function's unwind watermark; this is the same
            # guest-stack contract as the compiled tail-dispatch and keeps
            # LLE authoritative without inventing a C symbol or borrowing a
            # sibling decoded at a different M/X width.
            lines.append(
                f"      return interp_tier_dispatch_tail(cpu, "
                f"0x{tgt_addr:06x}u, 0x{site_pc24:06x}u, "
                f"_entry_s, _hrv); /* authoritative LLE M1X1 */")
        lines.append("    } break;")
    lines.append("    default: break;")
    lines.append("  }")
    lines.append("  return RECOMP_RETURN_NORMAL; /* dispatch is a terminator */")
    lines.append("}")
    return lines


def _emit_return_frame_push(op: 'Call') -> List[str]:
    """Option-1 cpu->S model: push the JSR/JSL return frame onto cpu->S
    (matching hardware) so the callee's RTS/RTL pops a real frame and
    trampoline / NLR returns resolve through cpu_dispatch_pc_from. Pushed
    value = (return_addr - 1); RTS/RTL add 1 on pop (matches the pop
    arithmetic in _emit_return). JSR pushes 2 bytes (16-bit, same bank);
    JSL pushes 3 bytes (PBR + 16-bit). When source_pc24 is unknown
    (synthesized call) push a correctly-SIZED sentinel — a balanced callee
    pops+ignores it; the rare trampoline dispatches to a lookup miss →
    NORMAL → host C unwind.

    MUST be paired with the always-pop _emit_return; and every OTHER
    invoke path (_emit_dispatch, indirect/tail emitters) must agree on
    push-vs-no-push or cpu->S leaks. See IMPROVEMENTS.md "Option-1".
    """
    site = (op.source_pc24 & 0xFFFFFF) if op.source_pc24 is not None else None
    # A direct generated JSR/JSL call always has a paired host-C caller +
    # a pushed return frame, so the callee enters with host_return_valid
    # = the pushed frame SIZE (JSR -> 2, JSL -> 3; see cpu_state.h).
    if op.long:
        ret16 = ((site + 3) & 0xFFFF) if site is not None else 0xFFFF
        pbr = ((site >> 16) & 0xFF) if site is not None else 0xFF
        return [
            "  /* JSL return frame -> cpu->S (Option-1) */",
            f"  cpu_write8(cpu, 0x00, cpu->S, 0x{pbr:02x}); cpu->S = (uint16)(cpu->S - 1);",
            f"  cpu_write8(cpu, 0x00, cpu->S, 0x{(ret16 >> 8) & 0xFF:02x}); cpu->S = (uint16)(cpu->S - 1);",
            f"  cpu_write8(cpu, 0x00, cpu->S, 0x{ret16 & 0xFF:02x}); cpu->S = (uint16)(cpu->S - 1);",
            "  cpu->host_return_valid = 3;  /* paired host caller, JSL frame */",
        ]
    ret16 = ((site + 2) & 0xFFFF) if site is not None else 0xFFFF
    return [
        "  /* JSR return frame -> cpu->S (Option-1) */",
        f"  cpu_write8(cpu, 0x00, cpu->S, 0x{(ret16 >> 8) & 0xFF:02x}); cpu->S = (uint16)(cpu->S - 1);",
        f"  cpu_write8(cpu, 0x00, cpu->S, 0x{ret16 & 0xFF:02x}); cpu->S = (uint16)(cpu->S - 1);",
        "  cpu->host_return_valid = 2;  /* paired host caller, JSR frame */",
    ]


def _emit_call(op: Call) -> List[str]:
    if op.indirect:
        # cfg-required-dispatch-or-kill (2026-05-03): JSR (abs,X) is
        # ONLY emitted as a real dispatch when cfg has authorised it
        # via an `indirect_call_table` directive. The decoder severs
        # the fall-through edge when no authorisation exists — see
        # decoder.SuppressedIndirectCall + the regression tests at
        # tests/v2/test_decoder_smc_phantom_suppression.py.
        #
        # The IR Call op is still produced for the suppressed JSR
        # (the predecessor block emits it as part of its lowering
        # output), but no fall-through code follows. The comment text
        # marks it as SUPPRESSED so cf_debt_report classifies it as a
        # suppressed phantom rather than a missing-dispatch priority.
        # Authorised JSR (abs,X) emit comes later (separate priority).
        if op.source_pc24 is not None and op.table_base is not None:
            return [
                f"/* Call indirect SUPPRESSED: JSR (${op.table_base:04X},X) at "
                f"${op.source_pc24:06X} — cfg-required-dispatch-or-kill, "
                f"no indirect_call_table authorisation */"
            ]
        return ["/* Call indirect SUPPRESSED — caller dispatches */"]
    if op.target is None:
        return ["/* Call: target unknown — caller dispatches */"]
    addr = op.target & 0xFFFFFF
    # Reject Calls whose target is structurally out of LoROM AND has no
    # cfg name. With a cfg name the user has explicitly declared an HLE
    # or hand-written backing (e.g. SmwRunDecompressFromWRAM at $7F:8000
    # is implemented in src/gen_stubs.c). Without a name, the JSL was
    # emitted because the decoder followed unreachable bytes past an
    # RTS and the operand bytes happened to look like a JSL — skipping
    # the emit avoids generating a trap stub for code that will never
    # actually run. To clean up the cfg `name`+`void` stub blocks for
    # similar dead-code targets, delete the cfg entries and re-regen —
    # this gate then rejects them in subsequent runs.
    if _is_invalid_lorom_call_target(addr) and addr not in _NAME_RESOLVER:
        _REJECTED_CALL_TARGETS.add(addr)
        return [f"/* Call: target ${addr:06X} not a valid LoROM code "
                f"address and no cfg name — skipped (decoder followed "
                f"garbage operand past an RTS) */"]
    base_name = _NAME_RESOLVER.get(addr)
    if base_name is None:
        bank = (addr >> 16) & 0xFF
        pc = addr & 0xFFFF
        base_name = f"bank_{bank:02X}_{pc:04X}"
    target_bank = (addr >> 16) & 0xFF
    # Runtime (m, x) dispatch: emit a 4-way switch on cpu->m_flag,
    # cpu->x_flag so the variant called matches the CPU's actual mode
    # at the JSR/JSL site. The decoder's static (m, x) tracking can
    # drift from runtime (PHP/PLP/RTI through unmodeled paths,
    # ambiguous callee exit states, etc.); the wrong-variant class
    # that surfaced via cpu_trace_mx_claim_check (Dr. Light capsule
    # freeze 2026-05-23) is eliminated by the dispatch — runtime
    # m/x ALWAYS picks the correct variant. Per-variant body emission
    # gets the operand widths right per (m, x); the unused variants
    # for a given runtime path are dead but compile cleanly. Demand
    # all 4 variants so v2_regen's autopromote synthesizes whichever
    # the static analysis missed — EXCEPT variants pruned by the
    # emit-truth prune pass (valid_variant_list), which we must not
    # re-demand or auto-promote would resurrect the garbage body.
    for em, ex in valid_variant_list(addr):
        _UNRESOLVED_CALL_TARGETS.add((addr, em, ex))
    call_trace_pc = f"0x{((op.source_pc24 or 0) & 0xFFFFFF):06x}u"
    if op.terminal:
        if op.long:
            raise ValueError("terminal call contract is only valid for direct JSR")
        # The hardware JSR frame is real data: inline dispatch helpers read it
        # to locate the table immediately following the call, then PLA it and
        # tail-transfer to the chosen state.  Push that frame exactly, but arm
        # the callee with THIS function's outer return ownership rather than a
        # paired two-byte host return.  There is deliberately no lexical
        # continuation and no SKIP_N decrement on the result.
        lines = ["{ /* terminal JSR: callee consumes inline return frame */"]
        lines += _emit_return_frame_push(op)
        lines += [
            "  cpu->host_return_valid = _hrv;",
            "  RecompReturn _r;",
            "  switch (((cpu->m_flag & 1) << 1) | (cpu->x_flag & 1)) {",
        ]
        lines += variant_dispatch_case_lines(
            addr, base_name,
            pre_call=[
                "cpu_tailcall_inherit_return_context(_entry_s, _hrv);"
            ],
            lle_fallback=(
                f"interp_tier_dispatch_tail(cpu, 0x{addr:06x}u, "
                f"{call_trace_pc}, _entry_s, _hrv)"))
        lines.extend([
            "  }",
            "  return _r;  /* terminal JSR transfers outer return ownership */",
            "}",
        ])
        return lines
    # cfg-pinned variant override. When the cfg names this call site
    # via `force_variant_at <site_pc24> <m> <x>`, bypass the 4-way
    # runtime switch and emit a hardcoded single-variant call. Used as
    # a diagnostic to validate suspected m-flag tracking bugs — see
    # cfg_loader.BankCfg.force_variant_at doc. Lookup keyed by the
    # JSR/JSL instruction's own PC24 (op.source_pc24), set by lowering.
    pinned = None
    if op.source_pc24 is not None:
        pinned = _FORCE_VARIANT_AT.get(op.source_pc24 & 0xFFFFFF)
    if pinned is not None:
        pm, px = pinned
        pinned_name = f"{base_name}{_variant_suffix(pm, px)}"
        if op.long:
            # JSL: keep the PB save/restore so the callee runs in its
            # target bank but the caller's PB is preserved on return.
            env = emitter_helpers.pb_save_restore_envelope(
                target_bank,
                [
                    f"RecompReturn _r = {pinned_name}(cpu);  "
                    f"/* cfg force_variant_at ${op.source_pc24:06X} -> "
                    f"M{pm}X{px} */",
                ],
                trace_pc24=op.source_pc24 or 0)
            return ["{", *[f"  {stmt}" for stmt in env], "}"]
        # JSR: same-bank short call. No PB change.
        return [
            "{",
            f"  RecompReturn _r = {pinned_name}(cpu);  "
            f"/* cfg force_variant_at ${op.source_pc24:06X} -> "
            f"M{pm}X{px} */",
            "  if (_r != RECOMP_RETURN_NORMAL) {",
            "    cpu_trace_event(cpu, 0, CPU_TR_NLR_PROPAGATE, (uint8)_r, 0);",
            "    cpu_trace_mark_nlr_exit(BD_EXIT_KIND_SKIP_PROPAGATION);",
            "    return (RecompReturn)((int)_r - 1);",
            "  }",
            "}",
        ]
    if op.long:
        # JSL: PB save/restore wraps the switch. The propagation
        # block sits AFTER the PB restore so the caller's PB is
        # correct on the SKIP_N return path.
        lines = ["{"]
        lines += _emit_return_frame_push(op)
        body = [
            "RecompReturn _r;",
            "switch (((cpu->m_flag & 1) << 1) | (cpu->x_flag & 1)) {",
        ]
        body += variant_dispatch_case_lines(
            addr, base_name, indent="  ",
            lle_fallback=(
                f"interp_tier_run_call_frame(cpu, 0x{addr:06x}u, "
                f"{call_trace_pc}, 3, NULL)"))
        body.append("}")
        env = emitter_helpers.pb_save_restore_envelope(
            target_bank, body, trace_pc24=op.source_pc24 or 0)
        lines += [f"  {stmt}" for stmt in env]
        lines.append("}")
        return lines
    # JSR: same-bank short call. PB doesn't change.
    # NB: emit_function.py's per-line scanner auto-injects a
    # RecompStackPop() before any line whose stripped text starts with
    # "return" — that includes the SKIP propagation `return` below.
    lines = ["{"]
    lines += _emit_return_frame_push(op)
    lines += [
        "  RecompReturn _r;",
        "  switch (((cpu->m_flag & 1) << 1) | (cpu->x_flag & 1)) {",
    ]
    lines += variant_dispatch_case_lines(
        addr, base_name,
        lle_fallback=(
            f"interp_tier_run_call_frame(cpu, 0x{addr:06x}u, "
            f"{call_trace_pc}, 2, NULL)"))
    lines.extend([
        "  }",
        "  if (_r != RECOMP_RETURN_NORMAL) {",
        "    cpu_trace_event(cpu, 0, CPU_TR_NLR_PROPAGATE, (uint8)_r, 0);",
        # Mark this exit as SKIP-PROPAGATION so the stack-drift
        # tripwire ignores the LEGITIMATE imbalance from skipping
        # this function's post-JSR cleanup (e.g. PLB) — by design
        # under the NLR ABI.
        "    cpu_trace_mark_nlr_exit(BD_EXIT_KIND_SKIP_PROPAGATION);",
        "    return (RecompReturn)((int)_r - 1);",
        "  }",
        "}",
    ])
    return lines


def _emit_return(op: Return) -> List[str]:
    """RTS / RTL / RTI emit. Reads + clears the function-LOCAL
    `_pending_skip` (set by an upstream NLR-pattern block on the same
    path) and returns its value. NORMAL paths get _pending_skip == 0
    == RECOMP_RETURN_NORMAL.

    `_pending_skip` is declared at the top of every emitted function
    by emit_function.py — see the prologue there for design rationale.
    NOT cpu->pending_skip: NLR signaling is C control-flow state, not
    65816 hardware state, and storing it on CpuState invited
    optimizer/aliasing weirdness around the cpu pointer.

    cpu_trace_pending_skip_consume is a non-rotating counter (separate
    from the cpu_trace ring, which rotates) so probes can answer "did
    any Return on this run read non-zero _pending_skip ever" — even
    after the ring has rotated past boot.

    The `return ...;` line stays at the start of its line so the
    per-line scanner in emit_function.py auto-injects RecompStackPop()
    before it.

    PEI-trampoline dispatch (2026-05-24, narrow variant). The post-
    lowering stack-delta analyser flags Return sites that are
    reachable with non-zero cpu->S delta from function entry (e.g.,
    the bank_04_9A02 PEI-trampoline in MMX). For those sites, codegen
    emits an extra branch: when `_pending_skip == NORMAL` AND the
    delta is non-zero at runtime, pop `frame_size` bytes from cpu->S,
    construct (PB:PC+1), tail-call cpu_dispatch_pc. The runtime check
    against `_entry_s` keeps the standard `return _ps;` path on the
    balanced subset of the function's paths (a single Return op may
    be reachable from both balanced and trampoline paths through the
    same join block — the static flag fires on either, but the
    runtime branch picks the right behaviour per execution).
    Reuses _entry_s captured in emit_function.py's prologue.
    """
    # Mark this exit as NLR-PRIMARY when _ps != NORMAL so the
    # boundary auditor's stack-drift tripwire ignores the
    # legitimate-imbalance case (an NLR-pattern block fired in this
    # function and the literal PLAs were skipped — entry_S == exit_S
    # in this codegen, but cpu_trace_mark_nlr_exit kept for
    # completeness / future-proofing for sub-classes of NLR).
    if op.interrupt:
        # Option-1 interrupt ABI: RTI pops the hardware interrupt frame that
        # the NMI/IRQ entry pushed (native: P, PCL, PCH, PB = 4 bytes;
        # emulation: P, PCL, PCH = 3 bytes). Restore P from the pulled byte
        # (like PLP); the pulled PC/PB are discarded — the host C return
        # carries control back to the scheduler (mmx_rtl.c) that invoked the
        # handler. MUST be paired with the interrupt-frame PUSH at the
        # I_NMI/I_IRQ invocation; without that push this over-pops cpu->S.
        return [
            "cpu_trace_event(cpu, 0, CPU_TR_RTI, 0, 0);",
            "{ cpu->S = (uint16)(cpu->S + 1); cpu->P = cpu_read8(cpu, 0x00, cpu->S); cpu_p_to_mirrors(cpu);",
            "  cpu->S = (uint16)(cpu->S + 2);  /* pull + discard PC */",
            "  if (!cpu->emulation) cpu->S = (uint16)(cpu->S + 1);  /* native: pull + discard PB */",
            "  cpu_trace_px_record(cpu, 0, 3 /*RTI*/, cpu->P, cpu->P);",
            "  return RECOMP_RETURN_NORMAL; /* RTI: popped interrupt frame */ }",
        ]
    # ── Option-1 cpu->S return-frame ABI (RTS / RTL) ──────────────────
    # ALWAYS pop the hardware return frame the matching JSR/JSL pushed
    # (RTS = 2 bytes, RTL = 3 bytes; pop adds 1 to the 16-bit part, matching
    # 65816 semantics + the _emit_return_frame_push value of return-1). Then:
    #   - host-return (RECOMP_RETURN_NORMAL) ONLY when a paired host-C caller
    #     exists (_hrv), the stack was balanced at entry (cpu->S == _entry_s
    #     before the pop), AND the popped return PC still matches the caller's
    #     frame.  Some games deliberately rewrite a JSR return address on the
    #     stack (for example Super Metroid's extension-block collision
    #     handlers); equal S alone must not discard that control-flow change.
    #   - otherwise (dispatched entry _hrv==0, OR a trampoline/NLR that
    #     changed the net stack so _ret_s != _entry_s) dispatch on the popped
    #     PC24. A deeper-than-entry RTS has popped a frame created by the
    #     current routine (the common PEI;RTS computed-dispatch idiom), so an
    #     unknown target must tier into LLE rather than be mistaken for a
    #     mid-caller return continuation. Shallower return continuations still
    #     use cpu_dispatch_pc_from's normal miss-unwind behavior.
    # This subsumes the old _TRAMPOLINE_RETURNS + _pending_skip paths — PLA*N
    # NLR now flows through the real cpu->S pops exposed below.
    label = "/* RTL */" if op.long else "/* RTS */"
    label_inner = "RTL" if op.long else "RTS"
    src24 = (op.source_pc24 or 0) & 0xFFFFFF
    lines = [
        f"{{ uint16 _ret_s = cpu->S;  /* {label_inner} pop hardware return frame */",
        "  cpu->S = (uint16)(cpu->S + 1);",
        "  uint16 _rpcl = (uint16)cpu_read8(cpu, 0x00, cpu->S);",
        "  cpu->S = (uint16)(cpu->S + 1);",
        "  uint16 _rpch = (uint16)cpu_read8(cpu, 0x00, cpu->S);",
    ]
    if op.long:
        lines.append("  cpu->S = (uint16)(cpu->S + 1);")
        lines.append("  uint8 _rpb = cpu_read8(cpu, 0x00, cpu->S);")
    else:
        lines.append("  uint8 _rpb = cpu->PB;")
    # Frame size this RTS/RTL pops (RTS = 2 bytes, RTL = 3). The dispatch
    # miss-restore below must reflect that this function popped its OWN return
    # frame, i.e. restore S to entry_s + frame_size — NOT bare entry_s.
    frame_sz = 3 if op.long else 2
    lines.extend([
        "  uint32 _rpc = (uint32)((((_rpch << 8) | _rpcl) + 1) & 0xFFFFu);",
        "  uint32 _rpc24 = ((uint32)_rpb << 16) | _rpc;",
        # task #7 RTS-decision trace (debug builds only). Records the exact
        # host-return-vs-dispatch-vs-miss classification at this RTS/RTL,
        # PC-range-filtered + tripwire-frozen in the recorder. Placed after
        # the pop so cpu->S == S-after-pop.
        "#if SNESRECOMP_TRACE",
        f"  dbg_rts_trace(cpu, 0x{src24:06x}u, _entry_s, _ret_s, _rpc24, (uint8)_hrv);",
        "#endif",
        f"  if (_hrv == {frame_sz} && _ret_s == _entry_s &&",
        "      _rpc24 != _host_return_pc24 && !cpu_dispatch_has_entry(cpu, _rpc24)) {",
        f"    return interp_tier_dispatch_rewritten_return(cpu, _rpc24, 0x{src24:06x}u);",
        "  }",
        f"  if (_hrv == {frame_sz} && _ret_s == _entry_s && _rpc24 == _host_return_pc24) {{",
        f"    return RECOMP_RETURN_NORMAL;  /* {label_inner} host return */ }}",
        # Return-to-ancestor (multi-level non-local return). When the stack
        # was manually rebalanced shallower than this frame's entry
        # (_ret_s != _entry_s, e.g. an OAM-overflow PLX/PLX/PLB epilogue)
        # and the popped PC is a host-return continuation (dispatch miss),
        # the RTS targets an ANCESTOR frame's continuation, not this
        # caller's. Resolve the ancestor by entry_s == _ret_s and unwind
        # to it via the existing SKIP_N decrement contract, so the ancestor
        # host-returns NORMAL and its caller resumes correctly. (A one-level
        # NORMAL miss-unwind here instead resumes intermediate frames that
        # hardware skipped — the fish-explosion OAM wipe; see ISSUES.md.)
        # Intentionally not gated by cpu_dispatch_has_entry(): a popped PC can
        # be both a valid continuation entry and a return-to-ancestor target.
        "  if (_ret_s != _entry_s) {",
        "    int _anc_skip = cpu_resolve_ancestor_skip(_ret_s);",
        "    if (_anc_skip >= 0) {",
        "      cpu_trace_mark_nlr_exit(BD_EXIT_KIND_TRAMPOLINE);",
        f"      return (RecompReturn)_anc_skip;  /* {label_inner} return-to-ancestor */ }}",
        # A skipped ancestor may be part of the active interpreter rather than
        # g_recomp_stack. Return the real popped continuation to that owning
        # bridge instead of creating a new dispatch root and then resuming code
        # the guest RTS already returned past. Test the architectural stack
        # direction, not whether the bounce frame size matches this final
        # return opcode: a JSR-bounced dispatcher may consume its two-byte
        # frame and tail into code that ultimately RTLs through an enclosing
        # interpreter-owned JSL frame (DKC2 sprite-state handlers). In that
        # case _hrv is 2 while this opcode's frame size is 3, but _ret_s is
        # still shallower than _entry_s and ownership belongs to the bridge.
        "    if ((uint16)(_ret_s - _entry_s) < 0x8000u &&",
        "        interp_bridge_has_direct_paired_bounce()) {",
        f"      return interp_tier_dispatch_rewritten_return(cpu, _rpc24, 0x{src24:06x}u); }}",
        "  }",
        # A computed RTS may push a synthetic target frame below entry and
        # consume it completely, leaving cpu->S exactly back at _entry_s.
        # During a direct interpreter->AOT bounce, that means the real paired
        # caller frame is still owned by the interpreter.  Hand the computed
        # continuation back to that owner.  Starting a popped-return bridge
        # here creates a nested interpreter whose watermark is _entry_s; if a
        # later helper non-locally returns through the preserved caller, the
        # nested bridge exits at the caller epilogue and the outer interpreter
        # executes that epilogue a second time (LttP sprite dispatch: PHA;RTS
        # selector followed by Sprite_ReturnIfInactive's PLA;PLA;RTS).
        "  if (_ret_s != _entry_s && cpu->S == _entry_s &&",
        "      interp_bridge_has_direct_paired_bounce()) {",
        f"    return interp_tier_dispatch_rewritten_return(cpu, _rpc24, 0x{src24:06x}u);",
        "  }",
        # A positive local delta smaller than this RTS/RTL frame consumes
        # local bytes plus part of the caller's return frame. The pop has
        # already crossed above entry S, so this is a rewritten/non-local
        # return, not an internal computed jump. Keep the continuation in the
        # interpreter and propagate SKIP_N; resuming the compiled caller would
        # execute guest code that the hardware return already crossed.
        "  if (_ret_s != _entry_s &&",
        "      (uint16)(_entry_s - _ret_s) < 0x8000u &&",
        "      cpu->S != _entry_s &&",
        "      (uint16)(cpu->S - _entry_s) < 0x8000u) {",
        f"    return interp_tier_dispatch_rewritten_return(cpu, _rpc24, 0x{src24:06x}u);",
        "  }",
        # The stack grows downward. If pre-RTS S is deeper than the routine's
        # entry S, the RTS consumed a synthetic frame pushed inside this
        # routine, not its caller's return frame. This is a live computed jump
        # even when its target was not discovered as a CFG root. Execute the
        # exact ROM continuation in LLE; treating an unknown target as the
        # usual mid-caller dispatch miss prematurely returns with locals (and
        # saved DB/P/Y state) still on the stack. The unsigned delta makes the
        # comparison safe across 16-bit S wrap for realistic (<32 KiB) frames.
        "  if (_ret_s != _entry_s &&",
        "      (uint16)(_entry_s - _ret_s) < 0x8000u &&",
        "      !cpu_dispatch_has_entry(cpu, _rpc24)) {",
        f"    return interp_tier_dispatch_popped_return(cpu, _rpc24, 0x{src24:06x}u,",
        f"        (uint16)(_entry_s + {frame_sz}u));",
        "  }",
        "  cpu_trace_mark_nlr_exit(BD_EXIT_KIND_TRAMPOLINE);",
        # Miss-restore = post-return-pop S (entry_s + frame_size). On a dispatch
        # MISS the popped PC is a normal return addr (mid-caller, not a function
        # entry), so this function must leave S as if it had popped its own
        # return frame. Passing bare entry_s under-pops by frame_size and leaks
        # the caller's frame on every miss — an hrv=0 callee dispatches on every
        # RTS, so under heavy load (hundreds of misses/frame) cpu->S drifts down
        # into zero page and corrupts the DMA queue tail -> 82C8/BA48 spin.
        f"  return cpu_dispatch_pc_from(cpu, _rpc24, (uint16)(_entry_s + {frame_sz}u), 0x{src24:06x}u);  /* {label_inner} dispatch */ }}",
    ])
    return lines


def _emit_stop(op: Stop) -> List[str]:
    if op.wait:
        # A compiled body can be entered as a paired bounce from the outer
        # interpreter scheduler.  WAI does not return through the guest JSR/JSL
        # frames beneath it: the next interrupt resumes the machine from this
        # architectural wait point.  Returning NORMAL here made the bounce
        # owner treat those still-live frames as an ordinary subroutine return,
        # resume at the caller's fall-through, and eventually corrupt S.
        #
        # Reuse the scheduler's existing depth-independent unwind sentinel.
        # Every generated callsite propagates it until the owning interpreter
        # frame consumes it and records the post-WAI PC as the next resume
        # point, matching the interpreter/hardware instruction advance.
        # Outside an LLE scheduler, preserve the established native-wrapper
        # behavior: the block's no-successor epilogue returns NORMAL.
        resume = (_CURRENT_SOURCE_PC24 + 1) & 0xFFFFFF
        return [
            "/* WAI: quiesce without guest-returning through live call frames */",
            "if (interp_bridge_in_lle_scheduler()) {",
            f"  return interp_bridge_lle_yield_unwind(cpu, 0x{resume:06x}u);",
            "}",
        ]
    return ["/* STP: halt — runtime hook */"]


def _emit_break(op: Break) -> List[str]:
    if not op.tier_to_lle:
        return ["/* COP: software interrupt */" if op.cop
                else "/* BRK: software interrupt */"]
    if op.source_pc24 is None:
        return ["/* software interrupt: missing source PC */"]
    site = op.source_pc24 & 0xFFFFFF
    kind = "COP" if op.cop else "BRK"
    return [
        f"/* {kind}: execute exact software interrupt in authoritative LLE */",
        f"return interp_tier_dispatch_tail(cpu, 0x{site:06x}u, "
        f"0x{site:06x}u, _entry_s, _hrv);",
    ]


def _emit_nop(op: Nop) -> List[str]:
    return ["/* NOP */"]


def _emit_pea_per_pei(op: PushEffectiveAddress) -> List[str]:
    # PEA/PER/PEI all push 16-bit immediates. Trace as PEA for now (kind
    # discrimination doesn't matter for stack-delta accounting).
    if op.seg.kind == SegKind.ABS_BANK:
        return [
            "{ uint16 _old_s = cpu->S;",
            "  cpu->S = (uint16)(cpu->S - 1);",
            f"  cpu_write16(cpu, 0x00, cpu->S, (uint16){op.seg.offset:#06x});",
            "  cpu->S = (uint16)(cpu->S - 1);",
            "  cpu_trace_stack_op(cpu, 0, CPU_STACK_OP_PEA, _old_s, -2); }",
        ]
    if op.seg.kind == SegKind.DP_INDIRECT:
        return [
            "{ uint16 _old_s = cpu->S;",
            f"  uint16 _peival = cpu_read16(cpu, 0x00, (uint16)(cpu->D + {op.seg.offset:#06x}));",
            "  cpu->S = (uint16)(cpu->S - 1);",
            "  cpu_write16(cpu, 0x00, cpu->S, _peival);",
            "  cpu->S = (uint16)(cpu->S - 1);",
            "  cpu_trace_stack_op(cpu, 0, CPU_STACK_OP_PEI, _old_s, -2); }",
        ]
    return ["/* TODO PushEffectiveAddress unsupported kind */"]


def _emit_blockmove(op: BlockMove) -> List[str]:
    delta = "+1" if op.direction == "mvn" else "-1"
    et = "CPU_TR_MVN" if op.direction == "mvn" else "CPU_TR_MVP"
    trace_pc = _trace_pc_arg()
    return [
        "{",
        f"  uint8 _src_b = {op.src_bank:#04x};",
        f"  uint8 _dst_b = {op.dst_bank:#04x};",
        "  uint8 _old_db = cpu->DB;",
        f"  cpu_trace_event(cpu, 0, {et}, _src_b, _dst_b);",
        "  while (cpu->A != 0xFFFF) {",
        "    uint8 _b = cpu_read8(cpu, _src_b, cpu->X);",
        "    cpu_write8(cpu, _dst_b, cpu->Y, _b);",
        f"    cpu->X = (uint16)(cpu->X {delta});",
        f"    cpu->Y = (uint16)(cpu->Y {delta});",
        "    cpu->A = (uint16)(cpu->A - 1);",
        "  }",
        "  cpu->DB = _dst_b;",
        f"  cpu_trace_db_change(cpu, {trace_pc}, _old_db, _dst_b, {et});",
        "}",
    ]


def _emit_push(op: Push) -> List[str]:
    # Generic Push IR (used for synthetic / non-register pushes). Trace as
    # PHA for accounting; the IR doesn't carry the original mnemonic.
    if op.width == 1:
        return emitter_helpers.stack_op_traced(
            "CPU_STACK_OP_PHA", -1,
            emitter_helpers.push_byte(f"(uint8){_v(op.src)}"))
    return emitter_helpers.stack_op_traced(
        "CPU_STACK_OP_PHA", -2,
        emitter_helpers.push_word(_v(op.src)))


def _emit_pull(op: Pull) -> List[str]:
    if op.width == 1:
        return emitter_helpers.stack_op_traced(
            "CPU_STACK_OP_PLA", +1,
            emitter_helpers.pop_byte_assign(f"uint8 {_v(op.out)}"))
    return emitter_helpers.stack_op_traced(
        "CPU_STACK_OP_PLA", +2,
        emitter_helpers.pop_word_assign(f"uint16 {_v(op.out)}"))


# ── Dispatch ────────────────────────────────────────────────────────────────

_DISPATCH = {
    Read: _emit_read, Write: _emit_write,
    ReadReg: _emit_readreg, WriteReg: _emit_writereg,
    ConstI: _emit_consti,
    Alu: _emit_alu, Shift: _emit_shift, IncReg: _emit_increg, IncMem: _emit_incmem,
    BitTest: _emit_bittest, BitSetMem: _emit_bitsetmem, BitClearMem: _emit_bitclearmem,
    SetFlag: _emit_setflag, SetNZ: _emit_setnz,
    RepFlags: _emit_repflags, SepFlags: _emit_sepflags,
    XCE: _emit_xce, XBA: _emit_xba,
    Push: _emit_push, Pull: _emit_pull,
    PushReg: _emit_pushreg, PullReg: _emit_pullreg,
    BlockMove: _emit_blockmove,
    CondBranch: _emit_condbranch, Goto: _emit_goto,
    IndirectGoto: _emit_indirect_goto, Call: _emit_call,
    Return: _emit_return, Transfer: _emit_transfer,
    Nop: _emit_nop, Break: _emit_break, Stop: _emit_stop,
    PushEffectiveAddress: _emit_pea_per_pei,
}


def emit_op(op: IROp, source_pc24: Optional[int] = None) -> List[str]:
    """Lower a single IR op to one or more lines of C."""
    h = _DISPATCH.get(type(op))
    if h is None:
        return [f"/* UNHANDLED IR op {type(op).__name__} */"]
    global _CURRENT_SOURCE_PC24
    old_source_pc24 = _CURRENT_SOURCE_PC24
    _CURRENT_SOURCE_PC24 = (int(source_pc24) & 0xFFFFFF) if source_pc24 is not None else 0
    try:
        return [ln for ln in h(op) if ln]
    finally:
        _CURRENT_SOURCE_PC24 = old_source_pc24


def emit_block(block: IRBlock, *, indent: str = "  ") -> List[str]:
    """Emit a list of indented C lines for one IRBlock.

    The block is wrapped in `{ ... }` so locals (introduced by ConstI,
    Read, ReadReg, Pull) don't leak across blocks.
    """
    lines = ["{"]
    for op in block.ops:
        for ln in emit_op(op):
            lines.append(indent + ln)
    lines.append("}")
    return lines
