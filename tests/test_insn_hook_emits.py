"""Pin: Tier-4 per-instruction hook (RDB_INSN_HOOK) emission and
runtime/recompiler mnemonic-table sync.

Every emitted 65816 instruction in --reverse-debug builds must be
preceded by an RDB_INSN_HOOK(pc, mnem_id) line. The mnem_id table in
recomp.py (INSN_MNEMONICS) must match the runtime table in
debug_server.c (s_insn_mnemonics[]) byte-for-byte; without that,
recorded log entries reference the wrong mnemonic name when probes
look them up.
"""
import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / 'recompiler'))

import recomp  # noqa: E402
from snes65816 import Insn, DP  # noqa: E402


def _build_ctx(reverse_debug):
    return recomp.EmitCtx(
        bank=0x00, func_names={}, ret_type='void',
        func_start=0x9870, reverse_debug=reverse_debug,
    )


def _emit_buf(ctx):
    for attr in ('lines', '_lines', 'body', '_body'):
        v = getattr(ctx, attr, None)
        if isinstance(v, list): return v
    raise AssertionError('emit buffer not found')


def _insn(mnem, mode=0, operand=0, addr=0x9870, length=2):
    i = Insn(addr=addr, opcode=0, mnem=mnem, mode=mode, operand=operand, length=length)
    i.m_flag = 1; i.x_flag = 1
    return i


def test_insn_hook_emits_for_stz():
    ctx = _build_ctx(True)
    ctx.emit(_insn('STZ', mode=DP, operand=0x100), branch_targets=set())
    out = '\n'.join(_emit_buf(ctx))
    expected_id = recomp.INSN_MNEM_TO_ID['STZ']
    # Macro signature: RDB_INSN_HOOK(pc, mnem, a, x, y, b, mx)
    assert f'RDB_INSN_HOOK(0x009870, {expected_id}, ' in out, \
        f'expected insn hook in:\n{out}'


def test_insn_hook_emits_for_inc():
    ctx = _build_ctx(True)
    ctx.emit(_insn('INC', mode=DP, operand=0x100), branch_targets=set())
    out = '\n'.join(_emit_buf(ctx))
    expected_id = recomp.INSN_MNEM_TO_ID['INC']
    assert f'RDB_INSN_HOOK(0x009870, {expected_id}, ' in out, \
        f'expected INC hook in:\n{out}'


def test_insn_hook_uses_unknown_id_for_unrecognized_mnem():
    ctx = _build_ctx(True)
    # Force a fake mnemonic that's not in the table.
    ctx.emit(_insn('ZZZ_FAKE'), branch_targets=set())
    out = '\n'.join(_emit_buf(ctx))
    assert 'RDB_INSN_HOOK(0x009870, 0, ' in out, \
        f'unknown mnemonic should map to id 0, got:\n{out}'


def test_insn_hook_emits_full_tracker_state_with_m_x_flags():
    ctx = _build_ctx(True)
    ctx.A = 'aval'; ctx.X = 'xval'; ctx.Y = 'yval'; ctx.B = 'bval'
    insn = _insn('NOP', mode=0, operand=0, length=1)
    insn.m_flag = 1  # 8-bit accumulator
    insn.x_flag = 0  # 16-bit index (so bit 1 is 0)
    ctx.emit(insn, branch_targets=set())
    out = '\n'.join(_emit_buf(ctx))
    # mx = m_flag bit | x_flag bit; insn.m_flag=1 (wide_a=False) so bit 0 set;
    # insn.x_flag=0 (wide_x=True) so bit 1 clear; mx=1.
    expected_mnem = recomp.INSN_MNEM_TO_ID['NOP']
    pattern = (f'RDB_INSN_HOOK(0x009870, {expected_mnem}, '
               '(uint32_t)(aval), (uint32_t)(xval), (uint32_t)(yval), '
               '(uint32_t)(bval), 1);')
    assert pattern in out, f'expected full-state hook:\n  {pattern}\nin:\n{out}'


def test_insn_hook_emits_unknown_for_untracked_regs():
    ctx = _build_ctx(True)
    ctx.A = None; ctx.X = None; ctx.Y = None; ctx.B = None
    insn = _insn('NOP', mode=0, operand=0, length=1)
    insn.m_flag = 1; insn.x_flag = 1   # both 8-bit -> mx = 0b11 = 3
    ctx.emit(insn, branch_targets=set())
    out = '\n'.join(_emit_buf(ctx))
    expected_mnem = recomp.INSN_MNEM_TO_ID['NOP']
    pattern = (f'RDB_INSN_HOOK(0x009870, {expected_mnem}, '
               'RDB_REG_UNKNOWN, RDB_REG_UNKNOWN, RDB_REG_UNKNOWN, '
               'RDB_REG_UNKNOWN, 3);')
    assert pattern in out, f'expected all-unknown hook with mx=3:\n  {pattern}\nin:\n{out}'


def test_no_insn_hook_in_non_reverse_debug():
    ctx = _build_ctx(False)
    ctx.emit(_insn('STZ', mode=DP, operand=0x100), branch_targets=set())
    out = '\n'.join(_emit_buf(ctx))
    assert 'RDB_INSN_HOOK' not in out, \
        f'non-reverse-debug build leaked RDB_INSN_HOOK:\n{out}'


def test_mnemonic_table_in_sync_with_runtime():
    """Parse debug_server.c's s_insn_mnemonics[] static array and
    compare to recomp.py's INSN_MNEMONICS. Any drift breaks the
    probe-side mnemonic name lookup."""
    runtime_c = (REPO / 'runner' / 'src' / 'debug_server.c').read_text()
    # Find the s_insn_mnemonics[] declaration and extract string list.
    m = re.search(r'static const char \*const s_insn_mnemonics\[\]\s*=\s*\{(.*?)\};',
                  runtime_c, flags=re.DOTALL)
    assert m, "could not locate s_insn_mnemonics[] in debug_server.c"
    body = m.group(1)
    runtime_table = tuple(re.findall(r'"([^"]+)"', body))
    py_table = recomp.INSN_MNEMONICS
    assert runtime_table == py_table, (
        "INSN_MNEMONICS drift between recomp.py and debug_server.c.\n"
        f"  runtime ({len(runtime_table)}): {runtime_table[:8]}...\n"
        f"  python  ({len(py_table)}):   {py_table[:8]}...")


def test_label_block_hook_and_insn_hook_both_emit_at_branch_target():
    """At a labeled block-target, BOTH the block hook and insn hook
    should fire (block hook first, then insn hook). Probes need both
    to correlate block-level boundaries with the per-insn stream."""
    ctx = _build_ctx(True)
    ctx.A = None; ctx.X = None; ctx.Y = None
    insn = _insn('NOP', mode=0, operand=0, addr=0x9870, length=1)
    ctx.emit(insn, branch_targets={0x9870})
    out = '\n'.join(_emit_buf(ctx))
    # Block hook line.
    assert 'RDB_BLOCK_HOOK(0x009870' in out, f'block hook missing at label:\n{out}'
    # Insn hook line (with extended args).
    nop_id = recomp.INSN_MNEM_TO_ID['NOP']
    assert f'RDB_INSN_HOOK(0x009870, {nop_id}, ' in out, \
        f'insn hook missing at label:\n{out}'
    # Order: block hook before insn hook.
    bh = out.index('RDB_BLOCK_HOOK(0x009870')
    ih = out.index('RDB_INSN_HOOK(0x009870')
    assert bh < ih, f'block hook must precede insn hook, got block@{bh} insn@{ih}'
