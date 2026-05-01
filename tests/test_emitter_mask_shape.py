"""Per-emitter mask-shape tests — DRY_REFACTOR.md Step 5.

Each test asserts that an emitter routes its width-bound operands
through `widths.masked` (i.e. the emitted C contains the canonical
"& 0xFF" / "& 0xFFFF" pattern, with the right one for the IR width).

Together with the lint at tools/lint_codegen_widths.py, this gives a
pair of constraints that mechanically prevent the next sibling-class
bug:
  - lint says: no raw mask literals outside widths.py
  - shape tests say: every emitter contains a mask of the expected
    width

Both must pass. Either failing means a future emitter forgot to mask,
or worse, a prior emitter regressed.
"""
import pathlib
import sys

# Set up sys.path the same way run_tests.py does so v2.* imports work
TESTS_DIR = pathlib.Path(__file__).resolve().parent
REPO_ROOT = TESTS_DIR.parent
sys.path.insert(0, str(REPO_ROOT / 'recompiler'))

from v2.codegen import (
    _emit_alu, _emit_shift, _emit_bittest, _emit_setnz,
    _emit_writereg, _emit_pullreg, _emit_transfer, _emit_increg,
    _emit_pushreg, _emit_read, _emit_write, _emit_incmem,
    _emit_bitsetmem, _emit_bitclearmem,
)
from v2.ir import (
    Alu, AluOp, Shift, ShiftOp, BitTest, SetNZ,
    WriteReg, PullReg, Transfer, IncReg, PushReg,
    Read, Write, IncMem, BitSetMem, BitClearMem,
    Reg, Value, SegRef, SegKind,
)


def _v(vid):
    return Value(vid=vid)


def _join(lines):
    return "\n".join(lines)


# ── ALU width masks ─────────────────────────────────────────────────────

def test_alu_cmp_8bit_masks_both_operands():
    out = _join(_emit_alu(Alu(op=AluOp.CMP, lhs=_v(1), rhs=_v(2),
                              width=1, out=None)))
    assert "& 0xFF" in out, f"missing 8-bit mask on CMP operands:\n{out}"
    assert "& 0xFFFF" not in out, f"unexpected 16-bit mask on CMP-8:\n{out}"


def test_alu_cmp_16bit_uses_16bit_mask():
    out = _join(_emit_alu(Alu(op=AluOp.CMP, lhs=_v(1), rhs=_v(2),
                              width=2, out=None)))
    assert "& 0xFFFF" in out, f"missing 16-bit mask on CMP-16:\n{out}"


def test_alu_add_8bit_masks_carry_path():
    out = _join(_emit_alu(Alu(op=AluOp.ADD, lhs=_v(1), rhs=_v(2),
                              width=1, out=_v(3))))
    # ADC needs both the operand mask AND the 8-bit carry-bit ($0x100).
    assert "& 0xFF" in out, f"missing 8-bit mask on ADC operands:\n{out}"
    assert "0x100" in out, f"missing 8-bit carry-bit constant:\n{out}"


def test_alu_sub_16bit_uses_carry_bit_10000():
    out = _join(_emit_alu(Alu(op=AluOp.SUB, lhs=_v(1), rhs=_v(2),
                              width=2, out=_v(3))))
    assert "0x10000" in out, f"missing 16-bit carry-bit constant:\n{out}"
    assert "& 0xFFFF" in out, f"missing 16-bit operand mask:\n{out}"


# ── Shift width masks ───────────────────────────────────────────────────

def test_shift_lsr_8bit_masks_src():
    out = _join(_emit_shift(Shift(op=ShiftOp.LSR, src=_v(1),
                                  width=1, out=_v(2))))
    assert "& 0xFF" in out, f"missing 8-bit mask on LSR src:\n{out}"


def test_shift_asl_16bit_masks_src_for_carry():
    out = _join(_emit_shift(Shift(op=ShiftOp.ASL, src=_v(1),
                                  width=2, out=_v(2))))
    # ASL-16 carry must read bit-15 of the masked 16-bit value.
    assert "& 0xFFFF" in out, f"missing 16-bit mask on ASL src:\n{out}"
    assert "0x8000" in out, f"missing 16-bit sign-bit:\n{out}"


def test_shift_ror_8bit_uses_8bit_mask():
    out = _join(_emit_shift(Shift(op=ShiftOp.ROR, src=_v(1),
                                  width=1, out=_v(2))))
    assert "& 0xFF" in out, f"missing 8-bit mask on ROR src:\n{out}"


# ── BIT width masks (latent bug fixed by widths layer) ─────────────────

def test_bit_8bit_masks_a_register():
    out = _join(_emit_bittest(BitTest(operand=_v(1), width=1)))
    # Pre-DRY: cpu->A & operand was un-masked, leaking B in m=1. The
    # DRY layer routes through widths.masked so cpu->A is now masked
    # to width=1.
    assert "& 0xFF" in out, f"missing 8-bit mask on BIT operand:\n{out}"
    assert "cpu->A" in out
    # The cpu->A reference must be inside a width-masked expression,
    # not a bare reference for the AND.
    assert "cpu->A & " not in out.replace("cpu->A & 0xFF", ""), (
        f"BIT must mask cpu->A through widths.masked, not bare AND:\n{out}")


def test_bit_16bit_uses_overflow_4000():
    out = _join(_emit_bittest(BitTest(operand=_v(1), width=2)))
    assert "0x4000" in out, f"missing 16-bit V-flag bit on BIT-16:\n{out}"


# ── SetNZ + Transfer + WriteReg ────────────────────────────────────────

def test_setnz_8bit_masks_src():
    out = _join(_emit_setnz(SetNZ(src=_v(1), width=1)))
    assert "& 0xFF" in out, f"missing 8-bit mask on SetNZ-8:\n{out}"


def test_setnz_emits_p_update():
    """The DRY layer routes set_nz through cpu->P update, fixing the
    asymmetry where some emitters skipped P sync."""
    out = _join(_emit_setnz(SetNZ(src=_v(1), width=1)))
    assert "cpu->P" in out, f"SetNZ must update cpu->P:\n{out}"


def test_writereg_a_m1_preserves_high_via_helper():
    """Verify A m=1 path uses preserve_high pattern (keep B, replace low)."""
    out = _join(_emit_writereg(WriteReg(reg=Reg.A, src=_v(1))))
    # Expected: when m_flag=1, ORs (cpu->A & 0xFF00) | (src & 0xFF)
    assert "& 0xFF00" in out, f"A m=1 must preserve high byte:\n{out}"
    assert "& 0xFF" in out, f"A m=1 must mask new low byte:\n{out}"


def test_writereg_x_x1_zero_extends():
    """Verify X x=1 zeros high byte (hw contract — fix b39e99b)."""
    out = _join(_emit_writereg(WriteReg(reg=Reg.X, src=_v(1))))
    # Should NOT preserve cpu->X high in the x=1 branch.
    if "x_flag" in out:
        x_branch = out.split("if (cpu->x_flag)")[1].split("else")[0]
        assert "& 0xFF00" not in x_branch, (
            f"X x=1 must zero-extend, not preserve high byte:\n{out}")
        assert "& 0xFF" in x_branch, f"X x=1 must mask low byte:\n{out}"


def test_increg_x_8bit_zero_extends():
    """INX/DEX in x=1 mode: result must be zero-extended."""
    out = _join(_emit_increg(IncReg(reg=Reg.X, delta=1)))
    if "x_flag" in out:
        x_branch = out.split("if (cpu->x_flag)")[1].split("} else")[0]
        # x=1 branch should NOT preserve high byte.
        assert "& 0xFF00" not in x_branch, (
            f"INX x=1 must not preserve high — hw zeros it:\n{out}")


def test_pullreg_a_m1_preserves_high():
    out = _join(_emit_pullreg(PullReg(reg=Reg.A)))
    if "m_flag" in out:
        m_branch = out.split("if (cpu->m_flag)")[1].split("} else")[0]
        assert "& 0xFF00" in m_branch, (
            f"PLA m=1 must preserve A high (B):\n{out}")


def test_pullreg_x_x1_zero_extends():
    out = _join(_emit_pullreg(PullReg(reg=Reg.X)))
    if "x_flag" in out:
        x_branch = out.split("if (cpu->x_flag)")[1].split("} else")[0]
        assert "& 0xFF00" not in x_branch, (
            f"PLX x=1 must zero-extend high:\n{out}")


def test_transfer_to_x_x1_zero_extends():
    out = _join(_emit_transfer(Transfer(src=Reg.A, dst=Reg.X)))
    if "x_flag" in out:
        x_branch = out.split("if (cpu->x_flag)")[1].split("} else")[0]
        assert "& 0xFF00" not in x_branch, (
            f"TAX x=1 must zero-extend X.high:\n{out}")


def test_pushreg_a_m1_pushes_low_byte_only():
    out = _join(_emit_pushreg(PushReg(reg=Reg.A)))
    if "m_flag" in out:
        m_branch = out.split("if (cpu->m_flag)")[1].split("} else")[0]
        # m=1 branch must use cpu_write8 + low-byte mask.
        assert "cpu_write8" in m_branch, f"PHA m=1 must push 1 byte:\n{out}"
        assert "& 0xFF" in m_branch, f"PHA m=1 must mask low byte:\n{out}"


# ── Memory-access dispatch (Follow-up A) ────────────────────────────────

def test_read_8bit_uses_cpu_read8():
    seg = SegRef(kind=SegKind.DIRECT, offset=0x10)
    out = _join(_emit_read(Read(seg=seg, width=1, out=_v(1))))
    assert "cpu_read8(" in out, f"Read width=1 must use cpu_read8:\n{out}"
    assert "cpu_read16" not in out, f"Read width=1 must NOT use cpu_read16:\n{out}"


def test_read_16bit_uses_cpu_read16():
    seg = SegRef(kind=SegKind.DIRECT, offset=0x10)
    out = _join(_emit_read(Read(seg=seg, width=2, out=_v(1))))
    assert "cpu_read16(" in out, f"Read width=2 must use cpu_read16:\n{out}"
    # cpu_read8 may appear in DP-indirect address resolution but NOT
    # for the actual data-byte read.


def test_write_8bit_uses_cpu_write8():
    seg = SegRef(kind=SegKind.DIRECT, offset=0x10)
    out = _join(_emit_write(Write(seg=seg, src=_v(1), width=1)))
    assert "cpu_write8(" in out, f"Write width=1 must use cpu_write8:\n{out}"


def test_write_16bit_uses_cpu_write16():
    seg = SegRef(kind=SegKind.DIRECT, offset=0x10)
    out = _join(_emit_write(Write(seg=seg, src=_v(1), width=2)))
    assert "cpu_write16(" in out, f"Write width=2 must use cpu_write16:\n{out}"


def test_incmem_8bit_uses_8bit_dispatch():
    seg = SegRef(kind=SegKind.DIRECT, offset=0x10)
    out = _join(_emit_incmem(IncMem(seg=seg, width=1, delta=1)))
    assert "cpu_read8(" in out, f"IncMem width=1 must read 8-bit:\n{out}"
    assert "cpu_write8(" in out, f"IncMem width=1 must write 8-bit:\n{out}"


def test_incmem_16bit_uses_16bit_dispatch():
    seg = SegRef(kind=SegKind.DIRECT, offset=0x10)
    out = _join(_emit_incmem(IncMem(seg=seg, width=2, delta=-1)))
    assert "cpu_read16(" in out, f"IncMem width=2 must read 16-bit:\n{out}"
    assert "cpu_write16(" in out, f"IncMem width=2 must write 16-bit:\n{out}"


def test_bitsetmem_16bit_dispatch():
    seg = SegRef(kind=SegKind.DIRECT, offset=0x10)
    out = _join(_emit_bitsetmem(BitSetMem(seg=seg, width=2)))
    assert "cpu_read16(" in out and "cpu_write16(" in out, (
        f"BitSetMem width=2 must use 16-bit read+write:\n{out}")


def test_bitclearmem_8bit_dispatch():
    seg = SegRef(kind=SegKind.DIRECT, offset=0x10)
    out = _join(_emit_bitclearmem(BitClearMem(seg=seg, width=1)))
    assert "cpu_read8(" in out and "cpu_write8(" in out, (
        f"BitClearMem width=1 must use 8-bit read+write:\n{out}")


# ── JSL bank save/restore envelope (Follow-up B) ────────────────────────

def test_call_with_pb_save_returns_six_statements():
    from v2.emitter_helpers import call_with_pb_save
    env = call_with_pb_save(0x05, "MyFn_M1X1")
    assert len(env) == 6, (
        f"call_with_pb_save must return 6 statements (got {len(env)}):\n{env}")
    # Order matters: save -> trace JSL -> set PB -> call -> trace RTL -> restore
    text = "\n".join(env)
    assert "_saved_pb = cpu->PB" in env[0], f"stmt 1 must save PB:\n{env[0]}"
    assert "CPU_TR_JSL" in env[1], f"stmt 2 must be JSL trace:\n{env[1]}"
    assert "cpu->PB = 0x05" in env[2], f"stmt 3 must set PB to target:\n{env[2]}"
    assert "MyFn_M1X1(cpu);" == env[3], f"stmt 4 must be the callee:\n{env[3]}"
    assert "CPU_TR_RTL" in env[4], f"stmt 5 must be RTL trace:\n{env[4]}"
    assert "cpu->PB = _saved_pb" in env[5], f"stmt 6 must restore PB:\n{env[5]}"


# ── Stack helpers (Follow-up C) ─────────────────────────────────────────

def test_push_byte_writes_then_decrements():
    from v2.emitter_helpers import push_byte
    env = push_byte("(uint8)cpu->A")
    assert len(env) == 2
    assert "cpu_write8" in env[0] and "cpu->S" in env[0]
    assert "cpu->S - 1" in env[1]


def test_push_word_decrements_writes_decrements():
    from v2.emitter_helpers import push_word
    env = push_word("cpu->A")
    assert len(env) == 3
    assert "cpu->S - 1" in env[0]
    assert "cpu_write16" in env[1]
    assert "cpu->S - 1" in env[2]


def test_pop_byte_increments_then_reads():
    from v2.emitter_helpers import pop_byte_assign
    env = pop_byte_assign("uint8 _v")
    assert len(env) == 2
    assert "cpu->S + 1" in env[0]
    assert "cpu_read8" in env[1] and "uint8 _v =" in env[1]


def test_pop_word_increments_reads_increments():
    from v2.emitter_helpers import pop_word_assign
    env = pop_word_assign("cpu->A")
    assert len(env) == 3
    assert "cpu->S + 1" in env[0]
    assert "cpu_read16" in env[1]
    assert "cpu->S + 1" in env[2]


# ── REP/SEP P-mirror sync (Follow-up D) ─────────────────────────────────

def test_rep_modify_p_via_mirrors_clears_bits():
    from v2.emitter_helpers import modify_p_via_mirrors
    env = modify_p_via_mirrors(0x30, "rep")
    assert len(env) == 5
    text = "\n".join(env)
    assert "_old_p = cpu->P" in env[0]
    assert "cpu_mirrors_to_p" in env[1]
    assert "& ~0x30" in env[2]                   # AND-NOT mask
    assert "cpu_p_to_mirrors" in env[3]
    assert "/*REP*/" in env[4] and "cpu_trace_px_record" in env[4]


def test_sep_modify_p_via_mirrors_sets_bits():
    from v2.emitter_helpers import modify_p_via_mirrors
    env = modify_p_via_mirrors(0x20, "sep")
    text = "\n".join(env)
    assert "| 0x20" in env[2]                    # OR mask
    assert "/*SEP*/" in env[4]


def test_modify_p_via_mirrors_rejects_bad_kind():
    from v2.emitter_helpers import modify_p_via_mirrors
    try:
        modify_p_via_mirrors(0x10, "xor")
    except ValueError:
        return
    assert False, "modify_p_via_mirrors must reject non-rep/sep kinds"
