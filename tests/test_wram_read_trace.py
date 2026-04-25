"""Pin: Tier-4 reads. _wram() and _wram16() emit RDB_LOAD8/16 macro
form when --reverse-debug, plain g_ram[X] / GET_WORD form otherwise.

Routes every WRAM read through the runtime trace ring without
changing non-debug binary output.
"""
import pathlib
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / 'recompiler'))

import recomp  # noqa: E402


def _ctx(reverse_debug):
    return recomp.EmitCtx(
        bank=0x00, func_names={}, ret_type='void',
        func_start=0x9870, reverse_debug=reverse_debug,
    )


def test_wram_read_uses_rdb_load8_in_reverse_debug():
    ctx = _ctx(True)
    expr = ctx._wram(0x72, '0')
    assert 'RDB_LOAD8(0x72)' in expr, f'expected RDB_LOAD8 in: {expr!r}'


def test_wram_read_uses_plain_array_in_non_reverse_debug():
    ctx = _ctx(False)
    expr = ctx._wram(0x72, '0')
    assert expr.startswith('g_ram[0x72]'), f'expected plain g_ram in: {expr!r}'
    assert 'RDB_LOAD8' not in expr


def test_wram_read_indexed_uses_rdb_load8_with_index_expr():
    ctx = _ctx(True)
    expr = ctx._wram(0x100, 'k')
    # Indexed reads wrap at 16 bits so k=0xFFFF stays in bank $7E
    # (Phase B fuzz: INC $10,X with X=0xFFFF). See test_wram_addr_wrap.
    assert 'RDB_LOAD8((uint16)(0x100 + k))' in expr, f'indexed read shape wrong: {expr!r}'


def test_wram16_read_uses_rdb_load16_in_reverse_debug():
    ctx = _ctx(True)
    expr = ctx._wram16(0x94, '0')
    assert 'RDB_LOAD16(0x94)' in expr, f'expected RDB_LOAD16 in: {expr!r}'


def test_wram16_read_uses_get_word_in_non_reverse_debug():
    ctx = _ctx(False)
    expr = ctx._wram16(0x94, '0')
    assert 'GET_WORD(g_ram + 0x94)' in expr, f'expected GET_WORD in: {expr!r}'
    assert 'RDB_LOAD16' not in expr
