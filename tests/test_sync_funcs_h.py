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
