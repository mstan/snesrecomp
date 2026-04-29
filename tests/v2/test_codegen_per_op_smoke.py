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
    op = ReadReg(reg=Reg.A, out=Value(vid=3))
    s = _joined(emit_op(op))
    assert "cpu->A" in s
    assert "_v3" in s


def test_writereg_emits_cpustate_assignment():
    op = WriteReg(reg=Reg.X, src=Value(vid=4))
    s = _joined(emit_op(op))
    assert "cpu->X" in s
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
    op = XBA()
    s = _joined(emit_op(op))
    assert "cpu->A" in s and "cpu->B" in s


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
    assert "cpu->X = cpu->A" in s


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
