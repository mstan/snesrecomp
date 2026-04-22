"""Pin: when --reverse-debug is active, STZ / STX / STY to WRAM must
emit RDB_STORE8(addr, val), NOT plain `g_ram[addr] = val`. Consecutive-
STZ chains inside RunPlayerBlockCode_00EEE1 (bank 00 $EF68 area) used
to bypass the Tier-1 write hook, hiding PlayerInAir / PlayerOnGround
/ SpinJumpFlag clears from the WRAM trace. That broke bug-#8
investigation entirely until the emu-side write hook revealed the
clear WAS happening — the recomp's own trace just couldn't see it.

This test drives the emit dispatch for STZ, STX, and STY in 8-bit
form with reverse_debug=True and asserts that every WRAM-target store
goes through RDB_STORE8.
"""
import pathlib
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / 'recompiler'))

import recomp  # noqa: E402
from snes65816 import Insn, DP, DP_X, DP_Y, ABS  # noqa: E402


def _build_ctx(reverse_debug):
    ctx = recomp.EmitCtx(
        bank=0x00,
        func_names={},
        ret_type='void',
        func_start=0xEF68,
        reverse_debug=reverse_debug,
    )
    # Prime register state: STX/STY need something to store.
    ctx.X = 'k'
    ctx.Y = 'k'
    return ctx


def _captured_emits(ctx):
    """Drain ctx's accumulated emissions. EmitCtx stores them in a list
    attribute — we access whichever internal collection the class uses."""
    # The class appends to self.body (or similar). Find it dynamically
    # so the test survives trivial renames.
    for attr in ('body', '_lines', 'lines', '_body'):
        val = getattr(ctx, attr, None)
        if isinstance(val, list):
            return val
    # Fall back: scan any list-valued attribute that contains strings.
    for name in dir(ctx):
        val = getattr(ctx, name, None)
        if isinstance(val, list) and val and isinstance(val[0], str):
            return val
    raise AssertionError("could not locate EmitCtx's emit buffer")


def _insn(mnem, mode, operand, addr=0xEF68):
    """Default 8-bit widths (m=1, x=1) are set in Insn.__init__."""
    i = Insn(addr=addr, opcode=0, mnem=mnem, mode=mode, operand=operand, length=2)
    i.m_flag = 1
    i.x_flag = 1
    return i


def _run_and_grep(reverse_debug, mnem, mode, operand):
    ctx = _build_ctx(reverse_debug)
    ctx.emit(_insn(mnem, mode, operand), branch_targets=set())
    return '\n'.join(_captured_emits(ctx))


def test_stz_dp_emits_rdb_store8_in_reverse_debug():
    out = _run_and_grep(True, 'STZ', DP, 0x72)
    assert 'RDB_STORE8(0x72, 0)' in out, f'expected RDB_STORE8 in:\n{out}'
    assert 'g_ram[0x72] = 0;' not in out, f'plain assign leaked in:\n{out}'


def test_stz_dp_emits_plain_assign_in_non_reverse_debug():
    """Non-debug builds must stay byte-for-byte identical to legacy."""
    out = _run_and_grep(False, 'STZ', DP, 0x72)
    assert 'g_ram[0x72] = 0;' in out, f'expected plain assign in:\n{out}'
    assert 'RDB_STORE8' not in out, f'reverse-debug leaked into non-debug:\n{out}'


def test_stx_dp_emits_rdb_store8_in_reverse_debug():
    out = _run_and_grep(True, 'STX', DP, 0x13EF)
    assert 'RDB_STORE8' in out, f'expected RDB_STORE8 for STX DP in:\n{out}'
    assert 'RDB_STORE8(0x13ef,' in out, f'STX DP addr mismatch in:\n{out}'


def test_sty_dp_emits_rdb_store8_in_reverse_debug():
    out = _run_and_grep(True, 'STY', DP, 0x77)
    assert 'RDB_STORE8' in out, f'expected RDB_STORE8 for STY DP in:\n{out}'
    assert 'RDB_STORE8(0x77,' in out, f'STY DP addr mismatch in:\n{out}'


def test_stx_dp_y_emits_rdb_store8_in_reverse_debug():
    out = _run_and_grep(True, 'STX', DP_Y, 0x80)
    assert 'RDB_STORE8' in out, f'expected RDB_STORE8 for STX DP_Y in:\n{out}'


def test_sty_dp_x_emits_rdb_store8_in_reverse_debug():
    out = _run_and_grep(True, 'STY', DP_X, 0x80)
    assert 'RDB_STORE8' in out, f'expected RDB_STORE8 for STY DP_X in:\n{out}'


def test_stz_abs_emits_rdb_store8_for_non_hw_reg():
    """ABS addressing to a non-hardware-register WRAM location must
    still route through the hook. Hardware registers ($2100-$21FF,
    $4200-$43FF) go through WriteReg and are out of scope."""
    out = _run_and_grep(True, 'STZ', ABS, 0x1800)  # well past $21xx/$42xx HW ranges
    assert 'RDB_STORE8(0x1800, 0)' in out, f'expected RDB_STORE8 in:\n{out}'


# ---- Read-modify-write hook coverage (INC/DEC/ASL/LSR/ROL/ROR/TSB/TRB) ----
# These were the SECOND class of blindspot beyond STZ/STX/STY: every
# `g_ram[X]++` / `--` / `<<= 1` etc bypassed the RDB_STORE8 hook. Bug #8
# investigation initially mis-attributed cause because GameMode advances
# (INC.W $100) didn't show up in the WRAM trace at all. These tests pin
# the fix.

def test_inc_dp_routes_through_rdb_store8_in_reverse_debug():
    out = _run_and_grep(True, 'INC', DP, 0x100)
    assert 'RDB_STORE8(0x100,' in out, f'INC DP did not route through hook:\n{out}'
    # Regression: must NOT emit the legacy in-place ++ form.
    assert 'g_ram[0x100]++;' not in out, f'INC legacy form leaked in:\n{out}'


def test_dec_dp_routes_through_rdb_store8_in_reverse_debug():
    out = _run_and_grep(True, 'DEC', DP, 0x100)
    assert 'RDB_STORE8(0x100,' in out, f'DEC DP did not route through hook:\n{out}'
    assert 'g_ram[0x100]--;' not in out, f'DEC legacy form leaked in:\n{out}'


def test_asl_dp_routes_through_rdb_store8_in_reverse_debug():
    out = _run_and_grep(True, 'ASL', DP, 0x72)
    assert 'RDB_STORE8(0x72,' in out, f'ASL DP did not route through hook:\n{out}'
    assert 'g_ram[0x72] <<= 1;' not in out, f'ASL legacy form leaked in:\n{out}'


def test_lsr_dp_routes_through_rdb_store8_in_reverse_debug():
    out = _run_and_grep(True, 'LSR', DP, 0x72)
    assert 'RDB_STORE8(0x72,' in out, f'LSR DP did not route through hook:\n{out}'
    assert 'g_ram[0x72] >>= 1;' not in out, f'LSR legacy form leaked in:\n{out}'


def test_rol_dp_routes_through_rdb_store8_in_reverse_debug():
    out = _run_and_grep(True, 'ROL', DP, 0x72)
    assert 'RDB_STORE8(0x72,' in out, f'ROL DP did not route through hook:\n{out}'


def test_ror_dp_routes_through_rdb_store8_in_reverse_debug():
    out = _run_and_grep(True, 'ROR', DP, 0x72)
    assert 'RDB_STORE8(0x72,' in out, f'ROR DP did not route through hook:\n{out}'


def test_tsb_dp_routes_through_rdb_store8_in_reverse_debug():
    out = _run_and_grep(True, 'TSB', DP, 0x72)
    assert 'RDB_STORE8(0x72,' in out, f'TSB DP did not route through hook:\n{out}'
    assert 'g_ram[0x72] |= ' not in out, f'TSB legacy form leaked in:\n{out}'


def test_trb_dp_routes_through_rdb_store8_in_reverse_debug():
    out = _run_and_grep(True, 'TRB', DP, 0x72)
    assert 'RDB_STORE8(0x72,' in out, f'TRB DP did not route through hook:\n{out}'
    assert 'g_ram[0x72] &= ' not in out, f'TRB legacy form leaked in:\n{out}'


def test_inc_dp_keeps_legacy_form_in_non_reverse_debug():
    """Non-reverse-debug builds must keep the in-place ++ form so the
    Release|x64 binary's compiled output is unchanged."""
    out = _run_and_grep(False, 'INC', DP, 0x100)
    assert 'g_ram[0x100] = ' in out or 'g_ram[0x100]++' in out, \
        f'non-debug INC lost its emit:\n{out}'
    assert 'RDB_STORE8' not in out, f'non-debug build leaked RDB_STORE8:\n{out}'
