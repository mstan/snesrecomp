"""Per-op smoke tests for v2 lowering. Sample one or two opcodes per
category and assert the produced IR shape is sensible."""
from _helpers import make_lorom_bank0  # noqa: E402

from snes65816 import decode_insn  # noqa: E402
from v2 import lowering  # noqa: E402
from v2.ir import (  # noqa: E402
    Read, Write, ReadReg, WriteReg, ConstI, Alu, AluOp, Shift, ShiftOp,
    IncReg, BitTest, SetFlag, RepFlags, SepFlags, XCE, XBA, PushReg, PullReg,
    CondBranch, Goto, IndirectGoto, Call, Return, Transfer, Nop, Break, Stop,
    Reg, SegKind, Value,
)


def _vf():
    counter = [0]
    def alloc():
        counter[0] += 1
        return Value(vid=counter[0])
    return alloc


def _decode(opcode_bytes: bytes, *, m=1, x=1):
    rom = bytearray(0x8000)
    rom[0:len(opcode_bytes)] = opcode_bytes
    insn = decode_insn(bytes(rom), 0, 0x8000, 0, m=m, x=x)
    # decode_insn doesn't stamp these; callers do (v2 decoder does it
    # automatically — for direct-decode tests we do it here).
    insn.m_flag = m
    insn.x_flag = x
    return insn


def test_sta_dp_emits_readreg_write():
    insn = _decode(bytes([0x85, 0x42]))  # STA $42
    ops = lowering.lower(insn, value_factory=_vf())
    assert any(isinstance(o, ReadReg) and o.reg == Reg.A for o in ops)
    write = next(o for o in ops if isinstance(o, Write))
    assert write.seg.kind == SegKind.DIRECT
    assert write.seg.offset == 0x42


def test_lda_long_emits_long_segref():
    insn = _decode(bytes([0xAF, 0x34, 0x12, 0x7E]))  # LDA $7E:1234
    ops = lowering.lower(insn, value_factory=_vf())
    read = next(o for o in ops if isinstance(o, Read))
    assert read.seg.kind == SegKind.LONG
    assert read.seg.offset == 0x1234
    assert read.seg.bank == 0x7E


def test_adc_imm_m1_uses_alu_add_width_1():
    insn = _decode(bytes([0x69, 0x05]), m=1)  # ADC #$05
    ops = lowering.lower(insn, value_factory=_vf())
    alu = next(o for o in ops if isinstance(o, Alu))
    assert alu.op == AluOp.ADD
    assert alu.width == 1


def test_cmp_imm_no_destination():
    insn = _decode(bytes([0xC9, 0x05]), m=1)
    ops = lowering.lower(insn, value_factory=_vf())
    alu = next(o for o in ops if isinstance(o, Alu))
    assert alu.op == AluOp.CMP
    assert alu.out is None


def test_inc_acc_emits_increg():
    insn = _decode(bytes([0x1A]))   # INC A
    ops = lowering.lower(insn, value_factory=_vf())
    assert ops == [IncReg(reg=Reg.A, delta=+1)]


def test_inx_emits_increg():
    insn = _decode(bytes([0xE8]))
    ops = lowering.lower(insn, value_factory=_vf())
    assert ops == [IncReg(reg=Reg.X, delta=+1)]


def test_asl_acc_emits_shift_acc():
    insn = _decode(bytes([0x0A]))
    ops = lowering.lower(insn, value_factory=_vf())
    sh = next(o for o in ops if isinstance(o, Shift))
    assert sh.op == ShiftOp.ASL
    assert any(isinstance(o, ReadReg) and o.reg == Reg.A for o in ops)
    assert any(isinstance(o, WriteReg) and o.reg == Reg.A for o in ops)


def test_bit_imm_emits_bittest():
    insn = _decode(bytes([0x89, 0x80]), m=1)
    ops = lowering.lower(insn, value_factory=_vf())
    assert any(isinstance(o, BitTest) for o in ops)


def test_sec_emits_setflag_c1():
    insn = _decode(bytes([0x38]))
    ops = lowering.lower(insn, value_factory=_vf())
    assert ops == [SetFlag(flag=Reg.C, value=1)]


def test_rep_emits_repflags_with_mask():
    insn = _decode(bytes([0xC2, 0x30]))
    ops = lowering.lower(insn, value_factory=_vf())
    assert ops == [RepFlags(mask=0x30)]


def test_sep_emits_sepflags_with_mask():
    insn = _decode(bytes([0xE2, 0x10]))
    ops = lowering.lower(insn, value_factory=_vf())
    assert ops == [SepFlags(mask=0x10)]


def test_xce_emits_xce():
    insn = _decode(bytes([0xFB]))
    ops = lowering.lower(insn, value_factory=_vf())
    assert ops == [XCE()]


def test_pha_emits_pushreg_a():
    insn = _decode(bytes([0x48]))
    ops = lowering.lower(insn, value_factory=_vf())
    assert ops == [PushReg(reg=Reg.A)]


def test_plx_emits_pullreg_x():
    insn = _decode(bytes([0xFA]))
    ops = lowering.lower(insn, value_factory=_vf())
    assert ops == [PullReg(reg=Reg.X)]


def test_beq_emits_condbranch_zf_1():
    insn = _decode(bytes([0xF0, 0x10]))
    ops = lowering.lower(insn, value_factory=_vf())
    assert ops == [CondBranch(flag=Reg.ZF, take_if=1)]


def test_bcc_emits_condbranch_c_0():
    insn = _decode(bytes([0x90, 0x10]))
    ops = lowering.lower(insn, value_factory=_vf())
    assert ops == [CondBranch(flag=Reg.C, take_if=0)]


def test_bra_emits_goto():
    insn = _decode(bytes([0x80, 0x10]))
    ops = lowering.lower(insn, value_factory=_vf())
    assert ops == [Goto()]


def test_jmp_abs_emits_goto():
    insn = _decode(bytes([0x4C, 0x34, 0x80]))
    ops = lowering.lower(insn, value_factory=_vf())
    assert ops == [Goto()]


def test_jmp_indir_emits_indirect_goto():
    insn = _decode(bytes([0x6C, 0x34, 0x12]))  # JMP ($1234)
    ops = lowering.lower(insn, value_factory=_vf())
    assert any(isinstance(o, IndirectGoto) for o in ops)


def test_jsr_abs_emits_call_short():
    insn = _decode(bytes([0x20, 0x34, 0x80]))  # JSR $8034
    ops = lowering.lower(insn, value_factory=_vf())
    assert len(ops) == 1
    assert isinstance(ops[0], Call)
    assert ops[0].long is False
    assert ops[0].target == 0x8034


def test_jsl_emits_call_long():
    insn = _decode(bytes([0x22, 0x34, 0x80, 0x7E]))  # JSL $7E:8034
    ops = lowering.lower(insn, value_factory=_vf())
    assert len(ops) == 1
    assert isinstance(ops[0], Call)
    assert ops[0].long is True


def test_rts_emits_return_short():
    insn = _decode(bytes([0x60]))
    ops = lowering.lower(insn, value_factory=_vf())
    assert ops == [Return(long=False)]


def test_rtl_emits_return_long():
    insn = _decode(bytes([0x6B]))
    ops = lowering.lower(insn, value_factory=_vf())
    assert ops == [Return(long=True)]


def test_rti_emits_return_interrupt():
    insn = _decode(bytes([0x40]))
    ops = lowering.lower(insn, value_factory=_vf())
    assert ops == [Return(long=True, interrupt=True)]


def test_tax_emits_transfer_a_x():
    insn = _decode(bytes([0xAA]))
    ops = lowering.lower(insn, value_factory=_vf())
    assert ops == [Transfer(src=Reg.A, dst=Reg.X)]


def test_xba_emits_xba():
    insn = _decode(bytes([0xEB]))
    ops = lowering.lower(insn, value_factory=_vf())
    assert ops == [XBA()]


def test_nop_emits_nop():
    insn = _decode(bytes([0xEA]))
    ops = lowering.lower(insn, value_factory=_vf())
    assert ops == [Nop()]


def test_brk_emits_break_not_cop():
    insn = _decode(bytes([0x00, 0x00]))
    ops = lowering.lower(insn, value_factory=_vf())
    assert len(ops) == 1
    assert isinstance(ops[0], Break)
    assert ops[0].cop is False


def test_cop_emits_break_cop():
    insn = _decode(bytes([0x02, 0x00]))
    ops = lowering.lower(insn, value_factory=_vf())
    assert len(ops) == 1
    assert isinstance(ops[0], Break)
    assert ops[0].cop is True


def test_stp_emits_stop():
    insn = _decode(bytes([0xDB]))
    ops = lowering.lower(insn, value_factory=_vf())
    assert ops == [Stop(wait=False)]


def test_wai_emits_stop_wait():
    insn = _decode(bytes([0xCB]))
    ops = lowering.lower(insn, value_factory=_vf())
    assert ops == [Stop(wait=True)]


def test_indexed_segref_carries_index_reg():
    """LDA $1234,X -> SegRef(kind=ABS_BANK, offset=0x1234, index=Reg.X)."""
    insn = _decode(bytes([0xBD, 0x34, 0x12]), m=1)  # LDA $1234,X
    ops = lowering.lower(insn, value_factory=_vf())
    read = next(o for o in ops if isinstance(o, Read))
    assert read.seg.kind == SegKind.ABS_BANK
    assert read.seg.offset == 0x1234
    assert read.seg.index == Reg.X


def test_dp_indirect_long_y_segref():
    """LDA [$05],Y -> SegRef(kind=DP_INDIRECT_LONG, offset=0x05, index=Reg.Y)."""
    insn = _decode(bytes([0xB7, 0x05]), m=1)
    ops = lowering.lower(insn, value_factory=_vf())
    read = next(o for o in ops if isinstance(o, Read))
    assert read.seg.kind == SegKind.DP_INDIRECT_LONG
    assert read.seg.offset == 0x05
    assert read.seg.index == Reg.Y


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
