"""Regression test: the emit-truth clean-sibling variant prune must
recognize a wrong-width variant whose body emitted the unresolved
IndirectGoto trap.

Root cause (2026-05-29, ALttP dispatch regression): a function entered
at a fixed index width (e.g. Dungeon_TryScreenEdgeTransition $02:885E,
X=1) gets cloned into wrong-width (X=0) variants under the bf8a34b
runtime-(m,x)-dispatch policy. At the wrong width, `LDX #imm` decodes
3 bytes instead of 2, swallowing the next opcode and fabricating a
phantom `JML [abs]` -> emit_function emits
`cpu_trace_dispatch_oob(..., 0xFFFF); /* unresolved IndirectGoto -- HLE
pending */`. That marker was NOT in v2_regen._STUB_MARKERS, so
_scan_dirty_variants never flagged the wrong-width variant as dirty and
the prune left it (and the 4-way switch case that dispatches into the
garbage) in place -> 141 unresolved-indirect sites vs the oracle's 4,
garbled sprites, and a room-transition softlock.

Fix: add 'unresolved IndirectGoto' to _STUB_MARKERS so the marker both
fails the stub-lint AND feeds the clean-sibling prune.

This file does its own sys.path setup so it runs standalone
(`python tests/v2/test_prune_unresolved_indirect_goto.py`) regardless of
the project's pytest invocation layout.
"""
import os
import sys
import types

_THIS = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.abspath(os.path.join(_THIS, os.pardir, os.pardir))
for _p in (os.path.join(_REPO, 'recompiler'), os.path.join(_REPO, 'tools')):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import v2_regen  # noqa: E402

PHANTOM_LINE = (
    "    return cpu_trace_dispatch_oob(cpu, 0x02887d, 0xFFFF); "
    "/* unresolved IndirectGoto -- HLE pending */")


def _make_parsed():
    """Minimal `parsed` shape: [(bank, path, cfg)] with cfg.entries each
    exposing .name / .start / .entry_m / .entry_x (the attributes
    _scan_dirty_variants reads via base_start)."""
    entry = types.SimpleNamespace(
        name='Dungeon_TryScreenEdgeTransition', start=0x885E,
        entry_m=1, entry_x=1)
    cfg = types.SimpleNamespace(entries=[entry])
    return [(0x02, 'bank02.cfg', cfg)]


def _make_results(bodies):
    """bodies: list of (suffix, body_lines). Build a per-bank `src` blob
    with each `RecompReturn <name><suffix>(CpuState *cpu) {` def."""
    src = []
    for suffix, body in bodies:
        src.append(
            f"RecompReturn Dungeon_TryScreenEdgeTransition{suffix}(CpuState *cpu) {{")
        src.extend(body)
        src.append("}")
    return [{'status': 'ok', 'bank': 0x02, 'src': '\n'.join(src)}]


def test_unresolved_indirect_goto_is_a_stub_marker():
    assert 'unresolved IndirectGoto' in v2_regen._STUB_MARKERS, (
        "the phantom dispatch_oob trap string must be a stub marker so the "
        "clean-sibling prune flags wrong-width variants and the lint fails "
        "loudly on genuine residue")


def test_brk_and_cop_are_stub_markers():
    assert 'BRK: software interrupt' in v2_regen._STUB_MARKERS
    assert 'COP: software interrupt' in v2_regen._STUB_MARKERS


def test_brk_variant_marked_dirty_clean_sibling_is_not():
    parsed = _make_parsed()
    results = _make_results([
        ('_M0X1', ["    /* BRK: software interrupt */"]),
        ('_M1X1', ["    RecompStackPop(); return RECOMP_RETURN_NORMAL;"]),
    ])
    dirty, emitted = v2_regen._scan_dirty_variants(results, parsed)
    addr = (0x02 << 16) | 0x885E
    assert (addr, 0, 1) in dirty
    assert (addr, 1, 1) not in dirty
    assert (addr, 0, 1) in emitted and (addr, 1, 1) in emitted


def test_phantom_variant_marked_dirty_clean_sibling_is_not():
    parsed = _make_parsed()
    # M1X0 (X=0) misdecodes -> phantom JML -> unresolved IndirectGoto.
    # M1X1 (X=1, the cfg-default canonical) decodes cleanly.
    results = _make_results([
        ('_M1X0', [PHANTOM_LINE]),
        ('_M1X1', ["    RecompStackPop(); return RECOMP_RETURN_NORMAL;"]),
    ])
    dirty, emitted = v2_regen._scan_dirty_variants(results, parsed)
    addr = (0x02 << 16) | 0x885E
    assert (addr, 1, 0) in dirty, (
        f"wrong-width X=0 variant must be dirty; got {sorted(dirty)}")
    assert (addr, 1, 1) not in dirty, (
        f"clean canonical X=1 variant must NOT be dirty; got {sorted(dirty)}")
    # emitted reports BOTH variants regardless of dirtiness.
    assert (addr, 1, 0) in emitted and (addr, 1, 1) in emitted


def test_prune_cfg_named_drops_wrong_width_with_clean_canonical():
    """A cfg-declared function (canonical M1X1) with a dirty X=0 clone
    and a clean canonical must have the X=0 clone pruned and the
    canonical kept."""
    addr = (0x02 << 16) | 0x885E
    canonical = {addr: {(1, 1)}}
    dirty = {(addr, 1, 0), (addr, 0, 0)}
    emitted = {(addr, 0, 0), (addr, 0, 1), (addr, 1, 0), (addr, 1, 1)}
    prunable = v2_regen._compute_prunable(dirty, emitted, canonical)
    assert (addr, 1, 0) in prunable and (addr, 0, 0) in prunable
    assert (addr, 1, 1) not in prunable  # canonical never pruned


def test_prune_autopromoted_drops_wrong_width_via_default_canonical():
    """An auto-promoted synthetic target (NO cfg canonical) entered at
    the default (1,1) width, with dirty X=0 clones and a clean (1,1),
    must have the clones pruned — the gap that left 67 unresolved-indirect
    sites after the first fix pass. Effective canonical defaults to
    (1,1) for auto-promoted bases, the same width a bare `func` gets."""
    addr = (0x02 << 16) | 0xB6CD  # bank_02_B6CD, synthetic, real in oracle
    canonical: dict = {}  # not cfg-declared
    dirty = {(addr, 0, 0), (addr, 1, 0)}
    emitted = {(addr, 0, 0), (addr, 0, 1), (addr, 1, 0), (addr, 1, 1)}
    prunable = v2_regen._compute_prunable(dirty, emitted, canonical)
    assert (addr, 0, 0) in prunable and (addr, 1, 0) in prunable, (
        f"auto-promoted wrong-width clones with a clean (1,1) canonical "
        f"must be prunable; got {sorted(prunable)}")
    assert (addr, 1, 1) not in prunable  # canonical never pruned


def test_prune_keeps_genuine_unresolved_at_canonical_width():
    """SOUNDNESS: a genuine RAM/computed-dispatch site whose unresolved
    trap lives at the (1,1) entry width — with marker-free (but garbage)
    wrong-width clones — must NOT be pruned. The dirty (1,1) is the
    effective canonical, so it is skipped, and the clean clones are not
    dirty so they are never candidates. Nothing is pruned; the genuine
    site survives as residue (matches the oracle, which also leaves
    bank_0A_D013 unresolved at its entry width)."""
    addr = (0x0A << 16) | 0xD013  # bank_0A_D013, genuine (oracle has it)
    canonical: dict = {}
    dirty = {(addr, 1, 1)}  # genuine unresolved at the (1,1) entry width
    # wrong-width clones emitted but marker-free (garbage that did not
    # happen to fabricate an unresolved IndirectGoto)
    emitted = {(addr, 0, 0), (addr, 0, 1), (addr, 1, 0), (addr, 1, 1)}
    prunable = v2_regen._compute_prunable(dirty, emitted, canonical)
    assert (addr, 1, 1) not in prunable, (
        "must NOT prune the genuine unresolved variant at its entry width")
    assert not prunable, f"nothing should be pruned here; got {sorted(prunable)}"


def test_prune_keeps_all_dirty_genuine_dispatch():
    """A genuine dispatch site dirty at EVERY emitted width (no clean
    canonical) must NOT be pruned; it remains as loud trap residue."""
    addr = (0x0A << 16) | 0xADA5  # bank_0A_ADA5, genuine (oracle has it)
    canonical: dict = {}
    dirty = {(addr, 0, 0), (addr, 0, 1), (addr, 1, 0), (addr, 1, 1)}
    emitted = set(dirty)  # all emitted variants are dirty
    prunable = v2_regen._compute_prunable(dirty, emitted, canonical)
    assert not prunable, (
        f"all-dirty genuine dispatch must remain; got {sorted(prunable)}")


def test_variant_refresh_scans_autopromoted_entries_for_callee_modes():
    """Auto-promoted functions must feed variant discovery on later passes.

    Regression shape from ALttP HUD: a synthetic caller discovered after
    the initial pass calls an already cfg-declared callee while M/X are
    M0X0. The refresh must add the callee's M0X0 entry before the
    valid-variant map restricts runtime switch cases to stale entries.
    """
    caller = types.SimpleNamespace(
        name='AutoCaller', start=0x8000, end=0x8004,
        entry_m=0, entry_x=0)
    callee = types.SimpleNamespace(
        name='KnownCallee', start=0x9000, end=0x9003,
        entry_m=1, entry_x=1)
    cfg = types.SimpleNamespace(entries=[caller, callee])
    parsed = [(0x00, 'bank00.cfg', cfg)]

    rom = bytearray([0xEA] * 0x8000)
    rom[0x0000:0x0004] = bytes([0x20, 0x00, 0x90, 0x60])
    rom[0x1000:0x1003] = bytes([0xE2, 0x30, 0x60])

    variants = {
        0x008000: {(0, 0)},
        0x009000: {(1, 1)},
    }
    decoded = set()
    added = v2_regen._discover_variants_from_current_entries(
        parsed, bytes(rom), variants, decoded, dispatch_helpers={})

    entry_keys = {
        (e.start & 0xFFFF, e.entry_m & 1, e.entry_x & 1)
        for e in cfg.entries
    }
    assert added == 1
    assert (0x9000, 0, 0) in entry_keys
    assert (0, 0) in variants[0x009000]


def test_variant_refresh_uses_learned_callee_exit_mx_for_post_call_site():
    """A post-JSR call site must be decoded under the callee's exit M/X.

    ALttP doorway regression shape: initial discovery saw only the
    caller's entry M1X1 state, then exit-M/X autoroute learned that an
    earlier JSR can return M0X1. The following JSR therefore needs its
    target's M0X1 variant before valid_variants restricts the runtime
    switch cases.
    """
    caller = types.SimpleNamespace(
        name='Caller', start=0x8000, end=0x8007,
        entry_m=1, entry_x=1)
    mutator = types.SimpleNamespace(
        name='Mutator', start=0x8100, end=0x8103,
        entry_m=1, entry_x=1)
    target = types.SimpleNamespace(
        name='DoorHandler', start=0x9000, end=0x9001,
        entry_m=1, entry_x=1)
    cfg = types.SimpleNamespace(entries=[caller, mutator, target])
    parsed = [(0x00, 'bank00.cfg', cfg)]

    rom = bytearray([0xEA] * 0x8000)
    rom[0x0000:0x0007] = bytes([
        0x20, 0x00, 0x81,  # JSR Mutator
        0x20, 0x00, 0x90,  # JSR DoorHandler
        0x60,              # RTS
    ])
    rom[0x0100:0x0103] = bytes([0xC2, 0x20, 0x60])  # REP #$20; RTS
    rom[0x1000:0x1001] = bytes([0x60])

    variants = {
        0x008000: {(1, 1)},
        0x008100: {(1, 1)},
        0x009000: {(1, 1)},
    }
    callee_exit_mx = {(0x008100, 1, 1): (0, 1)}
    added = v2_regen._discover_variants_from_current_entries(
        parsed, bytes(rom), variants, set(), dispatch_helpers={},
        callee_exit_mx=callee_exit_mx)

    entry_keys = {
        (e.start & 0xFFFF, e.entry_m & 1, e.entry_x & 1)
        for e in cfg.entries
    }
    assert added == 1
    assert (0x9000, 0, 1) in entry_keys
    assert (0, 1) in variants[0x009000]


def _parsed_caller_callee():
    """Two cfg-named functions in different banks: Module0F_SpotlightClose
    ($02:9982, M1X1 canonical) tail-calls LinkOam_Main ($0D:A18E, M1X1
    canonical). Mirrors the real ALttP dangling-reference link error."""
    spot = types.SimpleNamespace(
        name='Module0F_SpotlightClose', start=0x9982, entry_m=1, entry_x=1)
    link = types.SimpleNamespace(
        name='LinkOam_Main', start=0xA18E, entry_m=1, entry_x=1)
    return [
        (0x02, 'bank02.cfg', types.SimpleNamespace(entries=[spot])),
        (0x0D, 'bank0d.cfg', types.SimpleNamespace(entries=[link])),
    ]


def test_scan_variant_refs_extracts_calls_resolves_names():
    """_scan_variant_refs records every `Name_MmXx(cpu)` call, resolving
    cfg names and synthetic bank_BB_AAAA names to 24-bit addresses;
    def lines (`(CpuState *cpu)`) and self-references are excluded."""
    parsed = _parsed_caller_callee()
    results = [{'status': 'ok', 'bank': 0x02, 'src': '\n'.join([
        "RecompReturn Module0F_SpotlightClose_M1X0(CpuState *cpu) {",
        "  { RecompReturn _tc = LinkOam_Main_M1X0(cpu); return _tc; }",
        "}",
        "RecompReturn Module0F_SpotlightClose_M1X1(CpuState *cpu) {",
        "  RecompReturn _r = LinkOam_Main_M1X1(cpu);",
        "}",
    ])}]
    refs = v2_regen._scan_variant_refs(results, parsed)
    spot = 0x02 << 16 | 0x9982
    link = 0x0D << 16 | 0xA18E
    assert refs.get((spot, 1, 0)) == {(link, 1, 0)}, refs
    assert refs.get((spot, 1, 1)) == {(link, 1, 1)}, refs


def test_scan_variant_refs_resolves_synthetic_and_excludes_switch():
    """Synthetic bank_BB_AAAA targets resolve; a DIRECT tail-call is
    recorded but a runtime-(m,x) `case N:` switch dispatch is NOT (it
    references every width and never dangles — counting it cascades
    taint across the whole call graph)."""
    parsed = [(0x05, 'bank05.cfg', types.SimpleNamespace(entries=[
        types.SimpleNamespace(name=None, start=0xCD48, entry_m=1, entry_x=1),
        types.SimpleNamespace(name=None, start=0x850E, entry_m=1, entry_x=1),
        types.SimpleNamespace(name=None, start=0xF262, entry_m=1, entry_x=1),
    ]))]
    results = [{'status': 'ok', 'bank': 0x05, 'src': '\n'.join([
        "RecompReturn bank_05_CD48_M1X0(CpuState *cpu) {",
        "  RecompReturn _tc = bank_05_F262_M1X0(cpu);",  # direct -> recorded
        "        case 2: _r = bank_05_850E_M1X0(cpu); break;",  # switch -> excluded
        "}",
    ])}]
    refs = v2_regen._scan_variant_refs(results, parsed)
    cd48 = 0x05 << 16 | 0xCD48
    e850 = 0x05 << 16 | 0x850E
    f262 = 0x05 << 16 | 0xF262
    assert refs.get((cd48, 1, 0)) == {(f262, 1, 0)}, refs
    assert (e850, 1, 0) not in refs.get((cd48, 1, 0), set()), (
        "runtime-(m,x) switch case must be excluded from the direct-ref graph")


def test_scan_variant_refs_excludes_mx_switch_no_overtaint():
    """REGRESSION (over-taint): a function containing the runtime-(m,x)
    dispatch switch to a wrong-width clone must NOT itself be tainted.
    Counting switch cases as references cascaded taint across ~half of
    all variants and blocked the dangling-ref prune (the canonical of
    every dangling clone got tainted, so clean_canon failed)."""
    caller = 0x02 << 16 | 0x8000
    callee = 0x02 << 16 | 0x9000
    parsed = [(0x02, 'bank02.cfg', types.SimpleNamespace(entries=[
        types.SimpleNamespace(name=None, start=0x8000, entry_m=1, entry_x=1),
        types.SimpleNamespace(name=None, start=0x9000, entry_m=1, entry_x=1),
    ]))]
    results = [{'status': 'ok', 'bank': 0x02, 'src': '\n'.join([
        "RecompReturn bank_02_8000_M1X1(CpuState *cpu) {",
        "  switch (((cpu->m_flag & 1) << 1) | (cpu->x_flag & 1)) {",
        "    case 0: _r = bank_02_9000_M0X0(cpu); break;",
        "    case 3: _r = bank_02_9000_M1X1(cpu); break;",
        "    default: _r = RECOMP_RETURN_NORMAL; break;",
        "  }",
        "}",
    ])}]
    refs = v2_regen._scan_variant_refs(results, parsed)
    assert refs.get((caller, 1, 1)) is None, (
        f"switch-only caller must have NO direct refs; got {refs}")
    # callee_M0X0 is a dangling wrong-width clone, but the switch caller
    # must not be tainted by it.
    tainted = v2_regen._propagate_reference_taint(
        set(), refs, {(caller, 1, 1), (callee, 1, 1)}, {0x02}, set())
    assert (caller, 1, 1) not in tainted


def test_scan_variant_refs_ignores_alias_section_calls():
    """REGRESSION (SMW/ALttP non-convergence treadmill, 2026-06-02): the
    un-suffixed `void <name>(CpuState *cpu)` entry-point aliases that
    emit_bank appends AFTER every bank's bodies each forward to their own
    variant (`RecompReturn _r = <name>_MmXx(cpu);` — a _VARIANT_CALL_RE hit).
    The alias def line is NOT a `RecompReturn ..._MmXx` variant def, so the
    scanner must reset `cur` on it; otherwise EVERY alias call is attributed
    to whatever real RecompReturn body was emitted LAST in the bank. That
    body then carries hundreds of phantom edges (incl. any dirty-stub alias
    like FE60), gets falsely tainted + pruned, and pruning it shifts "last
    body" to the next victim — the prune never reaches a fixpoint."""
    parsed = [(0x01, 'bank01.cfg', types.SimpleNamespace(entries=[
        types.SimpleNamespace(name=None, start=0xE538, entry_m=0, entry_x=0),
        types.SimpleNamespace(name=None, start=0x9000, entry_m=1, entry_x=1),
        types.SimpleNamespace(name=None, start=0xFE60, entry_m=1, entry_x=1),
    ]))]
    results = [{'status': 'ok', 'bank': 0x01, 'src': '\n'.join([
        # last real body before the alias section — makes NO cross call
        "RecompReturn bank_01_E538_M0X0(CpuState *cpu) {",
        "  RecompStackPop(); return RECOMP_RETURN_NORMAL;",
        "}",
        # alias section — each void wrapper forwards to its own variant;
        # FE60 is a dirty stub, so without the fix E538_M0X0 absorbs it.
        "void bank_01_9000(CpuState *cpu) {",
        "  RecompReturn _r = bank_01_9000_M1X1(cpu);",
        "}",
        "void bank_01_FE60(CpuState *cpu) {",
        "  RecompReturn _r = bank_01_FE60_M1X1(cpu);",
        "}",
    ])}]
    refs = v2_regen._scan_variant_refs(results, parsed)
    e538 = 0x01 << 16 | 0xE538
    assert refs.get((e538, 0, 0)) is None, (
        "alias-section calls must NOT attribute to the last real body "
        f"(the alias-bleed non-convergence bug); got {refs}")


def test_reference_taint_prunes_dangling_caller_clone():
    """The blocking LNK2019 case: a clean wrong-width caller clone
    (Module0F_SpotlightClose_M1X0) tail-calls a callee variant
    (LinkOam_Main_M1X0) that was pruned / never emitted -> dangling.
    Taint must flag the caller clone, and the clean-canonical guard
    must prune it (M1X1 canonical clean). The canonical caller and the
    emitted M1X1 callee are kept."""
    spot = 0x02 << 16 | 0x9982
    link = 0x0D << 16 | 0xA18E
    refs = {(spot, 1, 0): {(link, 1, 0)}, (spot, 1, 1): {(link, 1, 1)}}
    # LinkOam_Main_M1X0 is NOT emitted (dangling); M1X1 of both is.
    emitted = {(spot, 1, 0), (spot, 1, 1), (link, 1, 1)}
    tainted = v2_regen._propagate_reference_taint(
        set(), refs, emitted, {0x02, 0x0D}, set())
    assert (spot, 1, 0) in tainted, sorted(tainted)
    assert (spot, 1, 1) not in tainted, "canonical caller must stay clean"
    canonical = {spot: {(1, 1)}, link: {(1, 1)}}
    prunable = v2_regen._compute_prunable(tainted, emitted, canonical)
    assert (spot, 1, 0) in prunable
    assert (spot, 1, 1) not in prunable and (link, 1, 1) not in prunable


def test_reference_taint_chains_through_dirty_direct_target():
    """Taint propagates transitively through DIRECT references: a clone
    that directly tail-calls an all-dirty (own-marker) target is itself
    tainted and pruned (clean canonical). The clean canonical clone,
    which does not make the dirty call, is kept."""
    a = 0x05 << 16 | 0xCD48
    b = 0x05 << 16 | 0x850E
    dirty = {(b, 1, 0), (b, 0, 0)}  # b all-dirty (own marker)
    # a's wrong-width clones DIRECTLY tail-call the dirty b; the clean
    # canonical (1,1) clone does not.
    refs = {(a, 1, 0): {(b, 1, 0)}, (a, 0, 0): {(b, 0, 0)}}
    emitted = {(a, 0, 0), (a, 0, 1), (a, 1, 0), (a, 1, 1),
               (b, 1, 0), (b, 0, 0)}
    tainted = v2_regen._propagate_reference_taint(
        dirty, refs, emitted, {0x05}, set())
    assert (a, 1, 0) in tainted and (a, 0, 0) in tainted
    assert (a, 1, 1) not in tainted, "clean canonical clone untainted"
    prunable = v2_regen._compute_prunable(tainted, emitted, {})  # synthetic
    assert (a, 1, 0) in prunable and (a, 0, 0) in prunable
    assert (a, 1, 1) not in prunable  # canonical (1,1) kept


def test_reference_taint_keeps_genuine_multiwidth_caller():
    """SOUNDNESS: a function genuinely reached at two widths, each
    calling a callee variant that IS emitted, must NOT be tainted —
    no reference dangles, so nothing is pruned."""
    a = 0x01 << 16 | 0x8000
    b = 0x01 << 16 | 0x9000
    refs = {(a, 1, 1): {(b, 1, 1)}, (a, 0, 1): {(b, 0, 1)}}
    emitted = {(a, 1, 1), (a, 0, 1), (b, 1, 1), (b, 0, 1)}
    tainted = v2_regen._propagate_reference_taint(
        set(), refs, emitted, {0x01}, set())
    assert not tainted, f"no dangling ref -> no taint; got {sorted(tainted)}"


def test_reference_taint_ignores_out_of_set_bank_targets():
    """A call into a bank NOT in the cfg set resolves to a loud stub
    body (unresolved_stubs_v2.c), never dangles. Such references must
    NOT taint the caller (else legitimate cross-ROM-region callers get
    pruned)."""
    a = 0x01 << 16 | 0x8000
    ext = 0x24 << 16 | 0x8000  # bank $24 not in cfg set
    refs = {(a, 1, 0): {(ext, 1, 0)}}
    emitted = {(a, 1, 0), (a, 1, 1)}  # ext never emitted (stubbed instead)
    tainted = v2_regen._propagate_reference_taint(
        set(), refs, emitted, {0x01}, set())
    assert (a, 1, 0) not in tainted, "out-of-set target must not taint"


def test_clean_noncanonical_direct_ref_protects_dirty_target_variant():
    """A clean non-canonical body can be a legitimate runtime-M/X target.

    Super Metroid shape: DemoSetFunc_2_M0X0 is clean and tail-calls
    DemoSetFunc_Common_M0X0. The callee body carries unresolved residue, but
    pruning it leaves the surviving clean caller hardwired to the M1X1
    callee, which misdecodes the 16-bit argument write.
    """
    source = (0x11 << 16) | 0x8A49
    target = (0x11 << 16) | 0x8A56
    refs = {(source, 0, 0): {(target, 0, 0)}}
    canonical = {source: {(1, 1)}, target: {(1, 1)}}
    dirty = {(target, 0, 0)}
    emitted = {
        (source, 0, 0), (source, 1, 1),
        (target, 0, 0), (target, 1, 1),
    }
    protected = v2_regen._protected_ref_targets(refs, canonical, dirty)
    assert (target, 0, 0) in protected
    prunable = v2_regen._compute_prunable(
        dirty, emitted, canonical, protected)
    assert (target, 0, 0) not in prunable


def test_dirty_noncanonical_direct_ref_does_not_protect_target_variant():
    """Wrong-width source residue must not keep its garbage call chain alive."""
    source = (0x11 << 16) | 0x8A49
    target = (0x11 << 16) | 0x8A56
    refs = {(source, 0, 0): {(target, 0, 0)}}
    canonical = {source: {(1, 1)}, target: {(1, 1)}}
    dirty = {(source, 0, 0), (target, 0, 0)}
    emitted = {
        (source, 0, 0), (source, 1, 1),
        (target, 0, 0), (target, 1, 1),
    }
    protected = v2_regen._protected_ref_targets(refs, canonical, dirty)
    assert (target, 0, 0) not in protected
    prunable = v2_regen._compute_prunable(
        dirty, emitted, canonical, protected)
    assert (target, 0, 0) in prunable


def test_partial_link_scan_includes_alias_and_unknown_synthetic_callers():
    """Preserved sources are link inputs, not merely decoded CFG bodies.

    Alias wrappers and synthetic callers missing from the current cfg are
    intentionally absent from the semantic reference-taint graph, but their
    calls still become linker relocations and must root regenerated variants.
    """
    parsed = _make_parsed()
    src = "\n".join([
        "void PreservedAlias(CpuState *cpu) {",
        "  Dungeon_TryScreenEdgeTransition_M0X0(cpu);",
        "}",
        "RecompReturn bank_00_9000_M1X1(CpuState *cpu) {",
        "  bank_02_A2A9_M1X1(cpu);",
        "}",
        "static RecompFunc preserved_ptr = bank_02_B2D4_M0X1;",
    ])
    results = [{'status': 'ok', 'bank': 0x00, 'src': src}]
    targets = v2_regen._scan_link_variant_targets(results, parsed)
    assert ((0x02 << 16) | 0x885E, 0, 0) in targets
    assert ((0x02 << 16) | 0xA2A9, 1, 1) in targets
    assert ((0x02 << 16) | 0xB2D4, 0, 1) in targets


def test_partial_link_root_prune_fails_immediately():
    root = ((0x02 << 16) | 0xA2A9, 1, 1)
    try:
        v2_regen._reject_partial_root_prune(
            {root}, {root}, "prune")
    except RuntimeError as exc:
        message = str(exc)
        assert "$02A2A9/M1X1" in message
        assert "preserved-bank link root" in message
    else:
        raise AssertionError("pruning a preserved-bank link root must fail")


def test_resolved_dispatch_default_is_not_a_false_positive():
    """A genuinely resolved indirect dispatch ends its switch with a
    `cpu_trace_dispatch_oob(..., _target)` default that has NO
    'unresolved IndirectGoto' comment. It must NOT be flagged dirty,
    else the prune would drop legitimate dispatcher variants."""
    parsed = _make_parsed()
    results = _make_results([
        ('_M1X1', [
            "    switch (_target) {",
            "      case 0x9391: { /* resolved */ } break;",
            "    }",
            "    return cpu_trace_dispatch_oob(cpu, 0x02885e, _target);",
        ]),
    ])
    dirty, _emitted = v2_regen._scan_dirty_variants(results, parsed)
    addr = (0x02 << 16) | 0x885E
    assert (addr, 1, 1) not in dirty, (
        f"resolved-dispatch default must not be flagged dirty; got {sorted(dirty)}")


if __name__ == '__main__':
    _tests = [v for k, v in sorted(globals().items())
              if k.startswith('test_') and callable(v)]
    for _t in _tests:
        _t()
    print(f"OK: all {len(_tests)} prune-marker regression assertions passed")
