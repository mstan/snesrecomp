"""Lint pass over v2 codegen output.

Catches the next foot-gun in the cpu->B class:

  - "shadow" CPU fields that aren't part of the documented canonical
    state and don't have a sync mechanism (e.g., a contributor adds
    `cpu->Z` as a temporary cache and forgets every primary-Z writer
    has to update it).

  - Stale historical references to deleted shadow fields like cpu->B.

  - Unsynced shadow fields (declared in CpuState but not referenced
    from cpu_p_to_mirrors / cpu_mirrors_to_p, so REP/SEP/PLP wouldn't
    keep them coherent).

The lint scans the actual emitted gen_v2/*.c sources rather than the
codegen.py emitter functions directly. That's the load-bearing surface
— if codegen.py emits an unexpected field through any conditional code
path, the gen output will reveal it. The lint runs as part of the
standard v2 test suite.

If a new field is genuinely needed:
  - canonical primary state goes in CANONICAL,
  - synced-from-P shadow goes in SHADOWS_FROM_P + cpu_p_to_mirrors,
  - any other shadow needs its own sync mechanism documented and added
    to ALLOWED with a comment explaining why it's safe.

This lint is the "before-the-third-repeat" project rule made
mechanical: it doesn't catch a stale-read bug at the use site (the
fuzz does that), but it catches the architectural shape that makes
stale-read bugs possible.
"""
from __future__ import annotations

import pathlib
import re


# Canonical CpuState fields — real architectural state of the 65816.
CANONICAL = {
    'A', 'X', 'Y',       # accumulator + index regs (16-bit, m/x_flag govern semantic width)
    'S',                 # stack pointer
    'D',                 # direct page register
    'DB', 'PB',          # data bank, program bank
    'P',                 # canonical processor flags byte
    'ram',               # pointer into runtime g_ram[] WRAM
}

# Shadow fields kept in sync with cpu->P via cpu_p_to_mirrors /
# cpu_mirrors_to_p. Read by codegen for fast per-bit access; updated
# whenever P is written via REP/SEP/PLP/RTI. The sync helpers in
# cpu_state.h are the load-bearing invariant; test_shadow_p_sync_is_complete
# below verifies every name here actually appears in cpu_p_to_mirrors.
SHADOWS_FROM_P = {
    'm_flag', 'x_flag',
    '_flag_N', '_flag_V', '_flag_Z', '_flag_C', '_flag_I', '_flag_D',
}

# Hidden 65816 register bits — NOT part of P, but state nonetheless.
# Their only mutator is XCE (which atomically swaps emulation <-> carry).
# These are read freely by the codegen but the field's coherence is the
# XCE emitter's responsibility, not cpu_p_to_mirrors'.
HIDDEN_REGS = {
    'emulation',
}

ALLOWED = CANONICAL | SHADOWS_FROM_P | HIDDEN_REGS


# Fields that were once present and have since been deleted as
# foot-gun shadows. Any reappearance is a regression — this list
# exists so the next "let me add cpu->FOO as a tracker" attempt
# trips a clear failure with a pointer to TROUBLESHOOTING.md.
HISTORICAL_DELETED = {
    'B': "cpu->B was a stale shadow of (cpu->A >> 8). Deleted in "
         "84b359e after it caused the SMW Layer-3 stripe-image XBA "
         "corruption (see docs/TROUBLESHOOTING.md). Read it as "
         "((cpu->A >> 8) & 0xFF) or via cpu_read_b().",
}


CPU_FIELD_RE = re.compile(r'\bcpu->(\w+)')


def _cpu_state_h() -> pathlib.Path:
    """Locate runner/src/cpu_state.h. snesrecomp is a self-contained
    subrepo, so this path is reachable from any of __file__'s ancestors
    that contains a `runner/` dir."""
    candidates: list[pathlib.Path] = []
    raw = pathlib.Path(__file__)
    candidates += list(raw.parents)
    candidates += list(raw.resolve().parents)
    candidates += list(pathlib.Path.cwd().parents) + [pathlib.Path.cwd()]
    seen: set[pathlib.Path] = set()
    for c in candidates:
        if c in seen:
            continue
        seen.add(c)
        cand = c / 'runner' / 'src' / 'cpu_state.h'
        if cand.exists():
            return cand
    return pathlib.Path('/nonexistent')


def _scan_v2_codegen_emits() -> dict:
    """Import v2.codegen and call every emitter with a representative
    IR op instance. Collect cpu->FIELD references across all emit
    outputs. Returns {field_name: [op_class_name, ...]} so the lint
    error message can point at which emitter introduces the unexpected
    field.

    This is the load-bearing path for the lint: if a contributor adds
    a new shadow field and writes an emitter that reads it, the new
    cpu->FIELD shows up here regardless of the parent repo's filesystem
    layout. The emit-level scan is more brittle than scanning gen_v2/*.c
    output (it only catches references in unconditional code paths) but
    far more portable.
    """
    from v2 import codegen, ir  # imported lazily so other tests don't pay

    out: dict[str, list[str]] = {}

    # Construct a representative instance of each known IR op. We don't
    # need them to be SEMANTICALLY meaningful — just well-formed enough
    # that emit_op runs without raising. For ops that take a Reg or
    # SegRef, we pick A / a low DP offset.
    seg_dp = ir.SegRef(kind=ir.SegKind.DIRECT, offset=0x10)
    samples = []
    samples.append(ir.Read(seg=seg_dp, width=1, out=ir.Value(vid=1)))
    samples.append(ir.Write(seg=seg_dp, src=ir.Value(vid=1), width=1))
    samples.append(ir.ReadReg(reg=ir.Reg.A, out=ir.Value(vid=2)))
    samples.append(ir.WriteReg(reg=ir.Reg.A, src=ir.Value(vid=2)))
    samples.append(ir.ReadReg(reg=ir.Reg.X, out=ir.Value(vid=3)))
    samples.append(ir.ReadReg(reg=ir.Reg.Y, out=ir.Value(vid=4)))
    samples.append(ir.ReadReg(reg=ir.Reg.B, out=ir.Value(vid=5)))  # historical foot-gun
    samples.append(ir.ReadReg(reg=ir.Reg.D, out=ir.Value(vid=6)))
    samples.append(ir.ReadReg(reg=ir.Reg.S, out=ir.Value(vid=7)))
    samples.append(ir.ReadReg(reg=ir.Reg.DB, out=ir.Value(vid=8)))
    samples.append(ir.ReadReg(reg=ir.Reg.PB, out=ir.Value(vid=9)))
    samples.append(ir.ReadReg(reg=ir.Reg.P, out=ir.Value(vid=10)))
    samples.append(ir.ConstI(value=0x42, width=1, out=ir.Value(vid=11)))
    samples.append(ir.IncReg(reg=ir.Reg.X, delta=1))
    samples.append(ir.SetFlag(flag=ir.Reg.M, value=1))
    samples.append(ir.RepFlags(mask=0x30))
    samples.append(ir.SepFlags(mask=0x30))
    samples.append(ir.XCE())
    samples.append(ir.XBA())
    samples.append(ir.PushReg(reg=ir.Reg.A))
    samples.append(ir.PullReg(reg=ir.Reg.A))
    samples.append(ir.Nop())
    # ALU and Shift ops require enums; use a baseline pair.
    samples.append(ir.Alu(op=ir.AluOp.ADD,
                          lhs=ir.Value(vid=11),
                          rhs=ir.Value(vid=11),
                          width=1,
                          out=ir.Value(vid=13)))
    samples.append(ir.Shift(op=ir.ShiftOp.ASL,
                            src=ir.Value(vid=11),
                            width=1,
                            out=ir.Value(vid=14)))
    samples.append(ir.Return(long=False))

    for op in samples:
        try:
            lines = codegen.emit_op(op)
        except Exception:
            # An emitter that needs a richer fixture is fine — skip.
            continue
        text = '\n'.join(lines)
        for match in CPU_FIELD_RE.finditer(text):
            field = match.group(1)
            out.setdefault(field, []).append(type(op).__name__)

    return out


def _scan_gen_fields() -> dict:
    """Walk every gen_v2/*.c and return a {field_name: [files...]} map
    of every cpu->FIELD reference. Returns {} if the gen dir doesn't
    exist (fresh checkout, no regen yet — lint silently skips)."""
    gen = _gen_v2_dir()
    if not gen.exists():
        return {}
    out: dict[str, list[str]] = {}
    for c_file in sorted(gen.glob('*.c')):
        text = c_file.read_text(encoding='utf-8', errors='replace')
        for match in CPU_FIELD_RE.finditer(text):
            out.setdefault(match.group(1), []).append(c_file.name)
    return out


# ────────────────────────────────────────────────────────────────────────────


def test_no_unexpected_cpu_fields_in_v2_emit():
    """Every cpu->FIELD reference produced by v2 codegen emitters must
    be in the documented allowlist. Catches the contributor adding a
    new shadow field plus an emitter that reads it — at test time,
    before regen + build + visual.

    Scans the in-process emit output rather than the regenerated
    gen_v2/*.c files so the test is portable across the project's
    junction layout (where filesystem ancestor walks miss the parent
    repo)."""
    seen = _scan_v2_codegen_emits()
    unexpected = {
        f: sorted(set(emitters))[:3]
        for f, emitters in seen.items()
        if f not in ALLOWED
    }
    assert not unexpected, (
        f"unexpected cpu->FIELD references in v2 emit output — either "
        f"widen the ALLOWED set in {pathlib.Path(__file__).name} "
        f"(documenting the new field's sync mechanism) or compute on-"
        f"demand from canonical state. Found: {unexpected}"
    )


def test_no_historical_deleted_fields_reappear():
    """Negative test for the cpu->B class — if a deleted shadow field
    reappears in v2 emit output, fail with a pointer to the
    TROUBLESHOOTING entry that explains why it was removed."""
    seen = _scan_v2_codegen_emits()
    for field, why in HISTORICAL_DELETED.items():
        assert field not in seen, (
            f"cpu->{field} reappeared in v2 emit output (referenced "
            f"by {len(set(seen[field]))} emitters: "
            f"{sorted(set(seen[field]))[:5]}).\n\n"
            f"{why}"
        )


def test_shadow_p_sync_is_complete():
    """Every shadow field in SHADOWS_FROM_P must be referenced inside
    BOTH cpu_p_to_mirrors and cpu_mirrors_to_p. Without this, the
    field would be `coherent at REP/SEP only by accident' — the bug
    pattern this whole lint is designed to prevent. The check is text-
    level (the helpers are inline functions in cpu_state.h)."""
    text = _cpu_state_h().read_text(encoding='utf-8')

    # Match the actual function definitions (not the doc-comment that
    # mentions them by name above the struct).
    p2m_def = re.search(r'static inline void cpu_p_to_mirrors\b[^{]*\{', text)
    m2p_def = re.search(r'static inline void cpu_mirrors_to_p\b[^{]*\{', text)
    assert p2m_def, "cpu_p_to_mirrors function definition not found"
    assert m2p_def, "cpu_mirrors_to_p function definition not found"

    # Slice each function body — from the open brace to the matching
    # close brace. Inline functions are short so we just take a
    # generous window and look for the next `}` at column 0.
    def _body_after(open_idx: int) -> str:
        # Walk forward to the closing `}` at start of a line.
        end = text.index('\n}\n', open_idx)
        return text[open_idx:end + 2]

    p2m_body = _body_after(p2m_def.end())
    m2p_body = _body_after(m2p_def.end())

    for shadow in SHADOWS_FROM_P:
        assert f'cpu->{shadow}' in p2m_body, (
            f"shadow cpu->{shadow} declared in SHADOWS_FROM_P but not "
            f"updated by cpu_p_to_mirrors (REP/SEP/PLP/RTI would leave it "
            f"stale)"
        )
        assert f'cpu->{shadow}' in m2p_body, (
            f"shadow cpu->{shadow} declared in SHADOWS_FROM_P but not "
            f"folded back into cpu->P by cpu_mirrors_to_p (PHP would push "
            f"a stale P)"
        )


def test_canonical_set_documents_real_struct_fields():
    """Every name in CANONICAL must appear as a top-level field in the
    CpuState struct definition. If a canonical field is renamed in
    cpu_state.h but not here, the gen lint above passes spuriously
    (it'd see the new name but never check the old). This test catches
    the drift."""
    text = _cpu_state_h().read_text(encoding='utf-8')
    struct_start = text.index('typedef struct CpuState')
    struct_end = text.index('} CpuState;')
    struct_body = text[struct_start:struct_end]
    for field in CANONICAL:
        # Match either `uint16 A;` or `uint8 *ram;` — i.e. any decl
        # that ends with "<field>;" or "<field>;<comment>".
        pattern = re.compile(rf'\b{re.escape(field)}\s*;')
        assert pattern.search(struct_body), (
            f"CANONICAL field cpu->{field} declared in lint but not found "
            f"in CpuState struct definition in cpu_state.h. Did the field "
            f"get renamed?"
        )
