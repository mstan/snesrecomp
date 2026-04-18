"""Tests for tools/sync_funcs_h.py auto-insert behavior.

Pins the invariant that sync_funcs_h inserts declarations for
recompiler-emitted functions that are missing from funcs.h. The prior
behavior was rewrite-only -- missing names were silently skipped,
forcing hand-edits to funcs.h (which violates Rule 7: generated files
are never hand-edited).

Scope note: this tool is a per-game script living in the game repo
(tools/sync_funcs_h.py), not the framework. Testing it from the
framework tests keeps the contract visible even though the test
doesn't exercise framework code directly.
"""
import importlib.util
import os
import pathlib
import sys
import tempfile


def _load_sync_funcs_h():
    """Import sync_funcs_h from the game repo under test."""
    # The test runs from inside snesrecomp/. The game repo sits one
    # level above and is the default SuperMarioWorldRecomp checkout.
    # If this test is run against a different game, it will skip.
    framework_root = pathlib.Path(__file__).resolve().parent.parent
    game_root = framework_root.parent
    script = game_root / 'tools' / 'sync_funcs_h.py'
    if not script.exists():
        return None
    # Inject game's snesrecomp/recompiler so sync_funcs_h's own imports work.
    sys.path.insert(0, str(framework_root / 'recompiler'))
    spec = importlib.util.spec_from_file_location('sync_funcs_h', str(script))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_sync_funcs_h_inserts_missing_declaration():
    sfh = _load_sync_funcs_h()
    if sfh is None:
        # Running against a game that has no sync_funcs_h.py; skip.
        return

    # Build a synthetic funcs.h that's missing a declaration the tool
    # is going to want to add. Use a name that won't already exist in
    # the real game's funcs.h so the test doesn't depend on game state.
    with tempfile.TemporaryDirectory() as tmpdir:
        funcs_h = pathlib.Path(tmpdir) / 'funcs.h'
        funcs_h.write_text(
            '#ifndef TEST_FUNCS_H_\n'
            '#define TEST_FUNCS_H_\n'
            '#include "smw_rtl.h"\n'
            'void ExistingHandwrittenHelper(void);\n'
            '#endif\n'
        )
        # Monkeypatch sync_funcs_h's FUNCS_H path and sig-collection
        # functions to feed it a known set.
        sfh.FUNCS_H = funcs_h

        def _fake_collect_cfg_sigs():
            return {}

        def _fake_collect_gen_sigs():
            return {
                0x008000: ('NewRecompilerFunc', 'void(uint8_k)'),
                0x008100: ('AnotherGenFunc', 'RetY(uint8_k,uint8_j)'),
            }

        sfh.collect_cfg_sigs = _fake_collect_cfg_sigs
        sfh.collect_gen_sigs = _fake_collect_gen_sigs

        rc = sfh.main()
        assert rc == 0

        out = funcs_h.read_text()

    # Both new declarations present.
    assert 'NewRecompilerFunc' in out, (
        f'tool did not insert NewRecompilerFunc\nout=\n{out}'
    )
    assert 'AnotherGenFunc' in out, (
        f'tool did not insert AnotherGenFunc\nout=\n{out}'
    )
    # Preserves hand-written declaration.
    assert 'ExistingHandwrittenHelper' in out, (
        f'tool removed hand-written declaration\nout=\n{out}'
    )
    # Final #endif still present.
    assert '#endif' in out


def test_sync_funcs_h_rebuilds_auto_block_on_rerun():
    """Running twice should not duplicate auto-inserted declarations."""
    sfh = _load_sync_funcs_h()
    if sfh is None:
        return
    with tempfile.TemporaryDirectory() as tmpdir:
        funcs_h = pathlib.Path(tmpdir) / 'funcs.h'
        funcs_h.write_text(
            '#ifndef TEST_FUNCS_H_\n'
            '#define TEST_FUNCS_H_\n'
            '#include "smw_rtl.h"\n'
            '#endif\n'
        )
        sfh.FUNCS_H = funcs_h
        sfh.collect_cfg_sigs = lambda: {}
        sfh.collect_gen_sigs = lambda: {
            0x008000: ('RerunFunc', 'void()'),
        }

        sfh.main()
        first = funcs_h.read_text()
        sfh.main()
        second = funcs_h.read_text()

    assert first == second, (
        f'second run produced different output\nfirst=\n{first}\nsecond=\n{second}'
    )
    # Only one instance of the declaration.
    assert second.count('void RerunFunc(void);') == 1, (
        f'declaration duplicated on rerun\nout=\n{second}'
    )


def test_sync_funcs_h_drops_stale_struct_return_when_no_hand_body():
    # When cfg explicitly declares a function void and gen emits a
    # body (but no hand body exists anywhere in src/*.c or cfg verbatim
    # blocks), funcs.h must track the cfg's void — not keep a stale
    # struct return from a prior hand-wrapper that's been deleted.
    #
    # This pins the fix for the tier-2 struct-return blocker: without
    # it, un-skipping a function whose hand wrapper returned a
    # synthetic struct leaves funcs.h perpetually declaring the struct
    # and callers fail to compile with "cannot convert from void to
    # StructT". With it, sync_funcs_h notices no hand body exists and
    # uses cfg's pre-gen-overlay sig directly, rewriting funcs.h to
    # match cfg; the next regen emits a void body and the cycle
    # converges.
    sfh = _load_sync_funcs_h()
    if sfh is None:
        return
    with tempfile.TemporaryDirectory() as tmpdir:
        funcs_h = pathlib.Path(tmpdir) / 'funcs.h'
        # Stale struct return from a deleted hand wrapper.
        funcs_h.write_text(
            '#ifndef TEST_FUNCS_H_\n'
            '#define TEST_FUNCS_H_\n'
            '#include "smw_rtl.h"\n'
            'PointU8 StaleStructFunc(uint8 k);\n'
            '#endif\n'
        )
        sfh.FUNCS_H = funcs_h
        # cfg has explicit void (e.g. cfg author verified against SMWDisX).
        # gen emitted PointU8 because _reconcile_sig picked funcs.h's
        # specificity; but with no hand body, sync_funcs_h should still
        # correct funcs.h to cfg's void and let the next regen converge.
        sfh.collect_cfg_sigs = lambda: {
            0x02D813: ('StaleStructFunc', 'void(uint8_k)'),
        }
        sfh.collect_gen_sigs = lambda: {
            0x02D813: ('StaleStructFunc', 'PointU8(uint8_k)'),
        }
        # No hand body anywhere: return an empty set.
        sfh.collect_hand_body_fnames = lambda: set()

        sfh.main()
        out = funcs_h.read_text()

    assert 'void StaleStructFunc(uint8 k)' in out, (
        f'sync_funcs_h did not correct stale struct return when no hand '
        f'body exists\nout=\n{out}'
    )
    assert 'PointU8 StaleStructFunc' not in out, (
        f'stale PointU8 declaration still present\nout=\n{out}'
    )


def test_sync_funcs_h_propagates_gen_params_when_no_hand_body():
    # When gen emits a widened param list via live-in inference (e.g.
    # `void f(uint8 j)` where cfg only had `void()`), and no hand body
    # exists to override the ABI, funcs.h must follow gen's params.
    # Otherwise hand-written callers that rely on funcs.h's declaration
    # pass too many arguments and fail to compile.
    sfh = _load_sync_funcs_h()
    if sfh is None:
        return
    with tempfile.TemporaryDirectory() as tmpdir:
        funcs_h = pathlib.Path(tmpdir) / 'funcs.h'
        funcs_h.write_text(
            '#ifndef TEST_FUNCS_H_\n'
            '#define TEST_FUNCS_H_\n'
            '#include "smw_rtl.h"\n'
            'void LiveInWidened(void);\n'
            '#endif\n'
        )
        sfh.FUNCS_H = funcs_h
        # cfg had void() (AUTO). Gen added `uint8 j` after live-in.
        sfh.collect_cfg_sigs = lambda: {
            0x009322: ('LiveInWidened', 'void()'),
        }
        sfh.collect_gen_sigs = lambda: {
            0x009322: ('LiveInWidened', 'void(uint8_j)'),
        }
        sfh.collect_hand_body_fnames = lambda: set()

        sfh.main()
        out = funcs_h.read_text()

    assert 'void LiveInWidened(uint8 j)' in out, (
        f'sync_funcs_h did not propagate gen live-in param (uint8 j) '
        f'to funcs.h\nout=\n{out}'
    )


def test_sync_funcs_h_drops_stale_pointer_param_when_no_hand_body():
    # When a previous hand body used a synthetic pointer parameter
    # (e.g. `ExtCollOut *out`) and funcs.h still declares it, un-skipping
    # the function should drop that param — pointers aren't derivable
    # from live-in analysis (live-in only adds register-style uint8
    # params named k/j/a/x/y). A pointer in gen's params must have been
    # inherited from funcs.h via _reconcile_sig's specificity choice;
    # without this filter the pointer param persists forever and
    # un-skipping is impossible.
    sfh = _load_sync_funcs_h()
    if sfh is None:
        return
    with tempfile.TemporaryDirectory() as tmpdir:
        funcs_h = pathlib.Path(tmpdir) / 'funcs.h'
        funcs_h.write_text(
            '#ifndef TEST_FUNCS_H_\n'
            '#define TEST_FUNCS_H_\n'
            '#include "smw_rtl.h"\n'
            'uint8 HandleExtFake(uint8 k, ExtCollOut *out);\n'
            '#endif\n'
        )
        sfh.FUNCS_H = funcs_h
        sfh.collect_cfg_sigs = lambda: {
            0x02A56E: ('HandleExtFake', 'uint8(uint8_k)'),
        }
        # Gen inherited the pointer param via _reconcile_sig's
        # specificity bias from funcs.h.
        sfh.collect_gen_sigs = lambda: {
            0x02A56E: ('HandleExtFake', 'uint8(uint8_k,ExtCollOut*_out)'),
        }
        sfh.collect_hand_body_fnames = lambda: set()

        sfh.main()
        out = funcs_h.read_text()

    assert 'uint8 HandleExtFake(uint8 k)' in out, (
        f'sync_funcs_h kept stale pointer param when no hand body '
        f'exists\nout=\n{out}'
    )
    assert 'ExtCollOut' not in out, (
        f'stale ExtCollOut pointer param still declared\nout=\n{out}'
    )


def test_sync_funcs_h_preserves_pointer_return_when_no_hand_body():
    # SNES functions that "return a pointer" actually communicate via
    # DP writes; the recompiler emits them as void bodies. funcs.h
    # declares them as pointer returns so hand callers (src/lm.c's
    # LmHook_GraphicsDecompress, for instance) can consume the DP-
    # stashed value as `uint8 *p = GraphicsDecompress(a);`.
    #
    # When un-skipping a different function forces funcs.h to pick cfg's
    # void, the pointer-return carve-out must still protect these: no
    # hand body exists for GraphicsDecompress, cfg says void, funcs.h
    # says uint8*. The pointer stays.
    sfh = _load_sync_funcs_h()
    if sfh is None:
        return
    with tempfile.TemporaryDirectory() as tmpdir:
        funcs_h = pathlib.Path(tmpdir) / 'funcs.h'
        funcs_h.write_text(
            '#ifndef TEST_FUNCS_H_\n'
            '#define TEST_FUNCS_H_\n'
            '#include "smw_rtl.h"\n'
            'uint8 *PointerReturnFunc(uint8 a);\n'
            '#endif\n'
        )
        sfh.FUNCS_H = funcs_h
        sfh.collect_cfg_sigs = lambda: {
            0x00BA28: ('PointerReturnFunc', 'void(uint8_a)'),
        }
        sfh.collect_gen_sigs = lambda: {
            0x00BA28: ('PointerReturnFunc', 'void(uint8_a)'),
        }
        sfh.collect_hand_body_fnames = lambda: set()

        sfh.main()
        out = funcs_h.read_text()

    assert 'uint8 *PointerReturnFunc(uint8 a)' in out or \
           'uint8* PointerReturnFunc(uint8 a)' in out, (
        f'pointer-return carve-out lost: sync_funcs_h dropped funcs.h '
        f'pointer even though the recompiler emits void body (pointer '
        f'value is DP-stashed and consumed by hand callers)\nout=\n{out}'
    )


def test_sync_funcs_h_preserves_struct_return_when_hand_body_exists():
    # Inverse of the above: when a hand body DOES exist (e.g. the
    # function is in cfg's verbatim block or in src/lm.c), the hand
    # body is the ABI oracle. funcs.h union path preserves the struct
    # return even if cfg somehow says void — the hand body, not the
    # recompiler, is authoritative here.
    sfh = _load_sync_funcs_h()
    if sfh is None:
        return
    with tempfile.TemporaryDirectory() as tmpdir:
        funcs_h = pathlib.Path(tmpdir) / 'funcs.h'
        funcs_h.write_text(
            '#ifndef TEST_FUNCS_H_\n'
            '#define TEST_FUNCS_H_\n'
            '#include "smw_rtl.h"\n'
            'PointU8 WrappedStructFunc(uint8 k);\n'
            '#endif\n'
        )
        sfh.FUNCS_H = funcs_h
        sfh.collect_cfg_sigs = lambda: {
            0x02D900: ('WrappedStructFunc', 'void(uint8_k)'),
        }
        sfh.collect_gen_sigs = lambda: {
            0x02D900: ('WrappedStructFunc', 'PointU8(uint8_k)'),
        }
        # Hand body exists (e.g. in cfg verbatim or src/smw_02.c).
        sfh.collect_hand_body_fnames = lambda: {'WrappedStructFunc'}

        sfh.main()
        out = funcs_h.read_text()

    assert 'PointU8 WrappedStructFunc(uint8 k)' in out, (
        f'sync_funcs_h dropped funcs.h struct return when hand body '
        f'still exists — hand body is the ABI oracle\nout=\n{out}'
    )
