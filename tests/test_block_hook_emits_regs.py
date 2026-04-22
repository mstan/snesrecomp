"""Pin: --reverse-debug RDB_BLOCK_HOOK calls now include the recomp's
abstract-tracked A/X/Y at the block-entry moment, plus the PC. The
runtime-side block-trace ring captures these so probes can inspect
the register state at any block hook (closes the visibility gap that
recomp's get_cpu_state only exposes per-frame snapshots, not
mid-frame block-level state).

Also covers: the EmitCtx._reg_for_hook helper round-trips both
known-expr and None values correctly.
"""
import pathlib
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / 'recompiler'))

import recomp  # noqa: E402
from snes65816 import Insn, DP  # noqa: E402


def _build_ctx(reverse_debug):
    ctx = recomp.EmitCtx(
        bank=0x00,
        func_names={},
        ret_type='void',
        func_start=0x9860,
        reverse_debug=reverse_debug,
        valid_branch_targets={0x9870},  # so emitting at $9870 produces a label hook
    )
    return ctx


def _captured_emits(ctx):
    for attr in ('lines', '_lines', 'body', '_body'):
        val = getattr(ctx, attr, None)
        if isinstance(val, list):
            return val
    raise AssertionError('could not locate EmitCtx emit buffer')


def test_reg_for_hook_returns_unknown_sentinel_for_none():
    ctx = _build_ctx(True)
    assert ctx._reg_for_hook(None) == 'RDB_REG_UNKNOWN'


def test_reg_for_hook_wraps_known_expr_in_uint32_cast():
    ctx = _build_ctx(True)
    assert ctx._reg_for_hook('k') == '(uint32_t)(k)'
    assert ctx._reg_for_hook('v17') == '(uint32_t)(v17)'
    # Compound expressions should also work — the cast is applied
    # outermost so any C expression is accepted.
    assert ctx._reg_for_hook('g_ram[0x72]') == '(uint32_t)(g_ram[0x72])'


def test_label_hook_includes_a_x_y_args_when_known():
    ctx = _build_ctx(True)
    # Manually set tracked register state, then drive an instruction
    # that lands on a labeled address ($9870 was registered as a
    # branch target above), forcing label-hook emission.
    ctx.A = 'a_val'
    ctx.X = 'x_val'
    ctx.Y = 'y_val'
    insn = Insn(addr=0x9870, opcode=0, mnem='NOP', mode=0, operand=0, length=1)
    insn.m_flag = 1; insn.x_flag = 1
    ctx.emit(insn, branch_targets={0x9870})
    out = '\n'.join(_captured_emits(ctx))
    assert 'RDB_BLOCK_HOOK(0x009870, (uint32_t)(a_val), (uint32_t)(x_val), (uint32_t)(y_val));' in out, \
        f'expected fully-typed RDB_BLOCK_HOOK in:\n{out}'


def test_label_hook_emits_unknown_sentinel_for_none_regs():
    ctx = _build_ctx(True)
    ctx.A = None
    ctx.X = None
    ctx.Y = None
    insn = Insn(addr=0x9870, opcode=0, mnem='NOP', mode=0, operand=0, length=1)
    insn.m_flag = 1; insn.x_flag = 1
    ctx.emit(insn, branch_targets={0x9870})
    out = '\n'.join(_captured_emits(ctx))
    assert 'RDB_BLOCK_HOOK(0x009870, RDB_REG_UNKNOWN, RDB_REG_UNKNOWN, RDB_REG_UNKNOWN);' in out, \
        f'expected unknown-sentinel RDB_BLOCK_HOOK in:\n{out}'


def test_label_hook_mixes_known_and_unknown_per_register():
    ctx = _build_ctx(True)
    ctx.A = None     # unknown
    ctx.X = 'k'      # known (typical X param)
    ctx.Y = None     # unknown
    insn = Insn(addr=0x9870, opcode=0, mnem='NOP', mode=0, operand=0, length=1)
    insn.m_flag = 1; insn.x_flag = 1
    ctx.emit(insn, branch_targets={0x9870})
    out = '\n'.join(_captured_emits(ctx))
    assert 'RDB_BLOCK_HOOK(0x009870, RDB_REG_UNKNOWN, (uint32_t)(k), RDB_REG_UNKNOWN);' in out, \
        f'expected mixed RDB_BLOCK_HOOK in:\n{out}'


def test_non_reverse_debug_emits_no_block_hook_at_label():
    ctx = _build_ctx(False)
    ctx.A = 'a'; ctx.X = 'k'; ctx.Y = 'j'
    insn = Insn(addr=0x9870, opcode=0, mnem='NOP', mode=0, operand=0, length=1)
    insn.m_flag = 1; insn.x_flag = 1
    ctx.emit(insn, branch_targets={0x9870})
    out = '\n'.join(_captured_emits(ctx))
    assert 'RDB_BLOCK_HOOK' not in out, \
        f'non-reverse-debug build leaked RDB_BLOCK_HOOK:\n{out}'
