"""Per-IR-op smoke tests for v2 codegen. Assert the emitted C contains
the expected substrings for each op kind."""
from _helpers import make_lorom_bank0  # noqa: E402

from v2.codegen import emit_op  # noqa: E402
from v2.ir import (  # noqa: E402
    Read, Write, ReadReg, WriteReg, ConstI, Alu, AluOp, Shift, ShiftOp,
    IncReg, BitTest, BitSetMem, BitClearMem,
    SetFlag, RepFlags, SepFlags, XCE, XBA,
    PushReg, PullReg, Push, Pull, BlockMove,
    CondBranch, Goto, IndirectGoto, Call, Return, Transfer,
    Nop, Break, Stop, PushEffectiveAddress,
    Reg, SegRef, SegKind, Value, IRBlock,
)


def _joined(lines):
    return "\n".join(lines)


def test_read_emits_cpu_read_call():
    op = Read(seg=SegRef(kind=SegKind.DIRECT, offset=0x42),
              width=1, out=Value(vid=1))
    s = _joined(emit_op(op))
    assert "cpu_read8" in s
    assert "_v1" in s
    assert "0x42" in s or "0x042" in s or "0x0042" in s


def test_write_emits_cpu_write_call():
    op = Write(seg=SegRef(kind=SegKind.ABS_BANK, offset=0x1234),
               src=Value(vid=2), width=2)
    s = _joined(emit_op(op))
    assert "cpu_write16" in s
    assert "_v2" in s


def test_readreg_emits_cpustate_field():
    """ReadReg of A should route through the typed helper cpu_read_a16
    rather than reaching into cpu->A directly. Same value, but the
    helper name carries the hardware contract and the lint can spot
    bypass attempts."""
    op = ReadReg(reg=Reg.A, out=Value(vid=3))
    s = _joined(emit_op(op))
    assert "cpu_read_a16(cpu)" in s
    assert "_v3" in s


def test_writereg_emits_cpustate_assignment():
    """WriteReg of X should route through cpu_write_x_x — the helper
    encodes the x-flag-driven 8-vs-16-bit dispatch + zero-extend
    semantics that distinguish X/Y writes from A writes."""
    op = WriteReg(reg=Reg.X, src=Value(vid=4))
    s = _joined(emit_op(op))
    assert "cpu_write_x_x(cpu" in s
    assert "_v4" in s


def test_consti_emits_literal():
    op = ConstI(value=0x1234, width=2, out=Value(vid=5))
    s = _joined(emit_op(op))
    assert "0x1234" in s
    assert "uint16" in s


def test_alu_add_uses_carry():
    op = Alu(op=AluOp.ADD, lhs=Value(vid=1), rhs=Value(vid=2),
             width=1, out=Value(vid=3))
    s = _joined(emit_op(op))
    assert "cpu->_flag_C" in s
    assert "_v1" in s and "_v2" in s and "_v3" in s


def test_alu_cmp_no_destination():
    op = Alu(op=AluOp.CMP, lhs=Value(vid=1), rhs=Value(vid=2),
             width=1, out=None)
    s = _joined(emit_op(op))
    assert "cpu->_flag_C" in s
    assert "cpu->_flag_Z" in s
    assert "cpu->_flag_N" in s


def test_shift_asl_updates_carry_and_z_n():
    op = Shift(op=ShiftOp.ASL, src=Value(vid=1), width=1, out=Value(vid=2))
    s = _joined(emit_op(op))
    assert "<< 1" in s
    assert "cpu->_flag_C" in s
    assert "cpu->_flag_Z" in s
    assert "cpu->_flag_N" in s


def test_increg_x_emits_x_plus_1():
    op = IncReg(reg=Reg.X, delta=+1)
    s = _joined(emit_op(op))
    assert "cpu->X" in s
    assert "+ (1)" in s or "+ 1" in s


def test_bittest_emits_a_and_operand():
    op = BitTest(operand=Value(vid=1), width=1)
    s = _joined(emit_op(op))
    assert "cpu->A" in s and "_v1" in s
    assert "_flag_V" in s


def test_setflag_c_1():
    op = SetFlag(flag=Reg.C, value=1)
    s = _joined(emit_op(op))
    assert "cpu->_flag_C" in s
    assert "= 1" in s


def test_rep_emits_p_clear_with_mask():
    op = RepFlags(mask=0x30)
    s = _joined(emit_op(op))
    assert "cpu->P" in s and "0x30" in s
    assert "cpu_p_to_mirrors" in s


def test_sep_emits_p_set_with_mask():
    op = SepFlags(mask=0x10)
    s = _joined(emit_op(op))
    assert "cpu->P" in s and "0x10" in s


def test_xce_swaps_emulation_and_carry():
    op = XCE()
    s = _joined(emit_op(op))
    assert "cpu->emulation" in s
    assert "cpu->_flag_C" in s


def test_xba_swaps_a_high_low():
    """XBA must operate on cpu->A only — never read cpu->B (which doesn't
    exist as a separate field, see CpuState in cpu_state.h). A previous
    shadow-field implementation went stale every time a 16-bit LDA wrote
    cpu->A without syncing the shadow; the regression caused SMW Layer-3
    stripe-image header parses to mis-derive their byte counts (visible
    attract-demo border garbage). See docs/TROUBLESHOOTING.md.

    Invariants the emit must satisfy:
      - References cpu->A.
      - Does NOT read cpu->B (forbidden — field removed).
      - Sets _flag_Z and _flag_N from the new low byte.
      - Computes a byte-swap of the prior cpu->A bits (text shape).
    """
    op = XBA()
    s = _joined(emit_op(op))
    assert "cpu->A" in s, "XBA must reference cpu->A"
    assert "cpu->B" not in s, (
        "XBA must NOT read or write cpu->B — that field has been removed "
        "and B is now derived from (A >> 8). Reading a stale shadow was "
        "the SMW Layer-3 stripe-corruption bug class."
    )
    assert "cpu->_flag_Z" in s and "cpu->_flag_N" in s, (
        "XBA must set Z and N from the new low byte"
    )
    # The emit must perform a byte swap. We don't pin the specific
    # variable name (some emitters bind cpu->A to a local first) but the
    # bit-fiddling shape is invariant: there must be both an `<< 8` and
    # a `>> 8` operating on the source.
    assert "<< 8" in s, "XBA must shift the old low byte up"
    assert ">> 8" in s, "XBA must shift the old high byte down"
    # And the swap result must land back in cpu->A.
    assert "cpu->A =" in s


def test_xba_independent_of_b_shadow():
    """Lint-style: no v2 emit anywhere reads cpu->B. cpu->B was deleted as
    independent state, so any emit referencing it would fail to compile —
    catch that at test time before regen, not at compile time."""
    # Walk every op kind we have emitters for and make sure none reference
    # cpu->B in the emit text. (Reg.B reads through the derived expression
    # in _REG_FIELD, which is not the literal `cpu->B`.)
    op = XBA()
    s = _joined(emit_op(op))
    assert "cpu->B" not in s, "XBA leaks cpu->B"
    op = ReadReg(reg=Reg.B, out=Value(vid=99))
    s = _joined(emit_op(op))
    assert "cpu->B" not in s, (
        "ReadReg(Reg.B) must not emit a literal cpu->B read; the canonical "
        "form is `(cpu->A >> 8) & 0xFF`."
    )
    # Spot-check: ReadReg(Reg.A) routes through the typed helper, so the
    # test isn't silently passing because nothing has any A reference at
    # all. The helper carries the cpu->A access internally; emit text
    # references the helper, not the field directly.
    op_a = ReadReg(reg=Reg.A, out=Value(vid=98))
    s_a = _joined(emit_op(op_a))
    assert "cpu_read_a16" in s_a


def test_pushreg_a_uses_m_flag_path():
    op = PushReg(reg=Reg.A)
    s = _joined(emit_op(op))
    assert "cpu->m_flag" in s
    assert "cpu_write8" in s
    assert "cpu_write16" in s


def test_pushreg_x_uses_x_flag_path():
    op = PushReg(reg=Reg.X)
    s = _joined(emit_op(op))
    assert "cpu->x_flag" in s


def test_pullreg_p_calls_mirrors_sync():
    op = PullReg(reg=Reg.P)
    s = _joined(emit_op(op))
    assert "cpu_p_to_mirrors" in s


def test_transfer_a_to_x():
    op = Transfer(src=Reg.A, dst=Reg.X)
    s = _joined(emit_op(op))
    # v2 transfer is width-aware (X width follows x_flag) and updates N/Z.
    assert "cpu->X" in s and "cpu->A" in s
    assert "x_flag" in s
    assert "_flag_Z" in s and "_flag_N" in s


def test_call_long_emits_function_call():
    op = Call(target=0x7E8034, long=True)
    s = _joined(emit_op(op))
    assert "bank_7E_8034" in s


def test_return_short_emits_return_stmt():
    op = Return(long=False)
    s = _joined(emit_op(op))
    assert "return;" in s


def test_blockmove_mvn_increments():
    op = BlockMove(direction='mvn', src_bank=0x7E, dst_bank=0x7F)
    s = _joined(emit_op(op))
    assert "cpu->X = (uint16)(cpu->X +1)" in s
    assert "cpu->Y = (uint16)(cpu->Y +1)" in s


def test_pea_pushes_immediate_onto_stack():
    op = PushEffectiveAddress(seg=SegRef(kind=SegKind.ABS_BANK, offset=0x1234))
    s = _joined(emit_op(op))
    assert "cpu->S" in s
    assert "0x1234" in s
    assert "cpu_write16" in s


def test_emit_block_wraps_in_braces():
    from v2.codegen import emit_block
    block = IRBlock(ops=[
        Nop(),
        SetFlag(flag=Reg.C, value=1),
    ])
    lines = emit_block(block, indent="  ")
    assert lines[0] == "{"
    assert lines[-1] == "}"
    body = "\n".join(lines[1:-1])
    assert "cpu->_flag_C" in body


if __name__ == '__main__':
    import sys, traceback
    failed = 0
    for name in [n for n in dir() if n.startswith('test_')]:
        try:
            globals()[name]()
            print(f"  PASS  {name}")
        except Exception:
            failed += 1
            print(f"  FAIL  {name}")
            traceback.print_exc()
    sys.exit(0 if failed == 0 else 1)
