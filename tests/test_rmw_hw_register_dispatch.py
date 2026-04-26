"""ASL / LSR / ROL / ROR / INC / DEC / TSB / TRB on absolute addresses
in the SNES HW-register range ($2100-$21FF, $4016-$4017,
$4200-$43FF) must route through ReadReg/WriteReg, not g_ram.

Before the fix, `_emit_rmw8` always used `_wram(addr, idx)` which
emits `RDB_LOAD8(addr)` / `g_ram[addr]` — for SMW's `ASL HW_RDMPY`
(at bank_02:11433, 11448, 16374, 16392 and bank_03:3303, 3321) this
read a random WRAM byte and wrote back to it instead of touching the
multiply hardware register. The post-RMW carry was effectively
random.

Audit context: docs/VIRTUAL_HW_AUDIT.md (the $4216 RMW gap).
"""
import pathlib
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / 'recompiler'))

import recomp  # noqa: E402
from snes65816 import decode_insn  # noqa: E402


def _decode_linear(rom: bytes, start_pc: int = 0x8000, m: int = 1, x: int = 1):
    out = []
    off = 0
    pc = start_pc
    while off < len(rom):
        insn = decode_insn(rom, off, pc, 0, m=m, x=x)
        if insn is None:
            break
        insn.m_flag = m
        insn.x_flag = x
        out.append(insn)
        if insn.mnem == 'REP':
            if insn.operand & 0x20: m = 0
            if insn.operand & 0x10: x = 0
        elif insn.mnem == 'SEP':
            if insn.operand & 0x20: m = 1
            if insn.operand & 0x10: x = 1
        off += insn.length
        pc = (pc + insn.length) & 0xFFFF
        if insn.mnem in ('RTS', 'RTL', 'RTI'):
            break
    return out


def _emit_body(rom: bytes) -> str:
    insns = _decode_linear(rom)
    lines = recomp.emit_function(
        name='test_fn', insns=insns, bank=0,
        func_names={}, func_sigs={},
        sig='void()', rom=rom,
    )
    return '\n'.join(lines)


def test_ASL_HW_RDMPY_uses_ReadReg_not_g_ram():
    """SMW's `ASL $4216` (ASL HW_RDMPY) must dispatch through ReadReg/
    WriteReg. The multiply hardware register is not in g_ram — reading
    g_ram[0x4216] gives a random WRAM byte (bank $7E offset $4216)."""
    # ASL $4216 ; RTS  (M=1 default, byte ASL)
    rom = bytes([
        0x0E, 0x16, 0x42,  # ASL $4216 (absolute)
        0x60,              # RTS
    ])
    body = _emit_body(rom)
    assert 'ReadReg(0x4216)' in body, (
        'ASL on $4216 must read via ReadReg, not g_ram; body:\n' + body)
    assert 'WriteReg(0x4216,' in body, (
        'ASL on $4216 must write via WriteReg, not g_ram; body:\n' + body)
    assert 'RDB_LOAD8(0x4216)' not in body, (
        'ASL on $4216 must not read from g_ram; body:\n' + body)
    assert 'g_ram[0x4216]' not in body, (
        'ASL on $4216 must not touch g_ram; body:\n' + body)


def test_INC_HW_register_uses_ReadReg():
    # INC $4202 ; RTS
    rom = bytes([
        0xEE, 0x02, 0x42,  # INC $4202 (HW_WRMPYA — absolute)
        0x60,              # RTS
    ])
    body = _emit_body(rom)
    assert 'ReadReg(0x4202)' in body, body
    assert 'WriteReg(0x4202,' in body, body


def test_LSR_DP_still_uses_wram():
    """RMW on direct page ($00-$FF) must still go through WRAM. Direct
    page is always $0000-$00FF and never overlaps HW registers (which
    start at $2100). This pins the regression: don't accidentally
    re-route DP-mode RMW through ReadReg."""
    rom = bytes([
        0x46, 0x00,        # LSR $00 (direct page)
        0x60,
    ])
    body = _emit_body(rom)
    assert 'ReadReg' not in body, (
        'LSR DP must not route through ReadReg; body:\n' + body)


def test_ASL_ABS_low_WRAM_still_uses_wram():
    """RMW on a low absolute address that ISN'T in HW range (e.g.
    $1234) must still hit WRAM via g_ram/RDB_LOAD8. Pins that the
    HW-range check doesn't over-trigger on plain WRAM addresses."""
    rom = bytes([
        0x0E, 0x34, 0x12,  # ASL $1234 (absolute, WRAM range)
        0x60,
    ])
    body = _emit_body(rom)
    assert 'ReadReg' not in body, (
        'ASL on plain WRAM addr must not use ReadReg; body:\n' + body)
    assert ('g_ram[0x1234]' in body or 'RDB_LOAD8(0x1234)' in body), (
        'ASL on $1234 must hit WRAM; body:\n' + body)


if __name__ == '__main__':
    test_ASL_HW_RDMPY_uses_ReadReg_not_g_ram()
    test_INC_HW_register_uses_ReadReg()
    test_LSR_DP_still_uses_wram()
    test_ASL_ABS_low_WRAM_still_uses_wram()
    print('all pass')
