"""Structural tests for sig-transformation passes in recomp.py.

Covers:
  - `_sig_matches_dispatch_shape`: gates which sigs are safe to keep for
    functions called via `FuncU8*` / `FuncV*` cast. Param count matters,
    return type does not.
  - Dispatch-target guard: once a function is in `cfg.dispatch_target_addrs`,
    live-in augmentation must not widen its param list beyond the
    dispatch shape, regardless of what it observes in the body.

These regressions caught two real crashes in SprStatus01_Init during
level-load dispatch of `SprXXX_Generic_Init_StandardSpritesInit`:

  (1) The augment pass added a second `uint8 a` param based on live-in
      analysis, but the `FuncU8*` cast at the dispatch call site only
      passes `k`, leaving `a` as uninitialised register state.
  (2) An earlier narrowing attempt also stripped RetAY returns from
      dispatch targets — which regressed direct-JSR callers (they
      consume the Y part of RetAY, and stripping the return dropped it
      to uint8, degrading emit to `= 0 /* UNKNOWN */`). The current
      guard only narrows params, not return type.
"""
import pathlib
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / 'recompiler'))

import recomp  # noqa: E402


# ---------------------------------------------------------------------------
# Issue B: FuncU8J / FuncU8A / FuncU8JA union-sig dispatch
# ---------------------------------------------------------------------------

def test_dispatch_typedef_for_shape_covers_all_combinations():
    # Every reachable shape maps to its typedef.
    assert recomp._dispatch_typedef_for_shape(frozenset()) == ('FuncV', [])
    assert recomp._dispatch_typedef_for_shape(frozenset({'k'})) == ('FuncU8', ['k'])
    assert recomp._dispatch_typedef_for_shape(frozenset({'k', 'j'})) == ('FuncU8J', ['k', 'j'])
    assert recomp._dispatch_typedef_for_shape(frozenset({'k', 'a'})) == ('FuncU8A', ['k', 'a'])
    assert recomp._dispatch_typedef_for_shape(frozenset({'k', 'j', 'a'})) == ('FuncU8JA', ['k', 'j', 'a'])


def test_dispatch_typedef_upgrades_orphan_j_or_a_to_k_form():
    # {'j'} / {'a'} alone (no k) route through the k-including typedef
    # — keeps all handlers sharing a typedef family; the handler simply
    # ignores the extra k argument.
    assert recomp._dispatch_typedef_for_shape(frozenset({'j'})) == ('FuncU8J', ['k', 'j'])
    assert recomp._dispatch_typedef_for_shape(frozenset({'a'})) == ('FuncU8A', ['k', 'a'])
    assert recomp._dispatch_typedef_for_shape(frozenset({'j', 'a'})) == ('FuncU8JA', ['k', 'j', 'a'])


def test_sig_matches_dispatch_shape_with_union_accepts_kj():
    # With allow_shape={k,j}, a (uint8_k, uint8_j) handler is valid.
    assert recomp._sig_matches_dispatch_shape(
        'void(uint8_k,uint8_j)', allow_shape=frozenset({'k', 'j'})) is True
    # Narrower sigs still fit the wider allow.
    assert recomp._sig_matches_dispatch_shape(
        'void(uint8_k)', allow_shape=frozenset({'k', 'j'})) is True
    assert recomp._sig_matches_dispatch_shape(
        'void()', allow_shape=frozenset({'k', 'j'})) is True


def test_sig_matches_dispatch_shape_rejects_out_of_order():
    # Param order must be canonical k, j, a.
    assert recomp._sig_matches_dispatch_shape(
        'void(uint8_j,uint8_k)', allow_shape=frozenset({'k', 'j'})) is False


def test_sig_matches_dispatch_shape_rejects_param_outside_shape():
    # allow_shape={k} → (uint8_k, uint8_j) doesn't fit.
    assert recomp._sig_matches_dispatch_shape(
        'void(uint8_k,uint8_j)', allow_shape=frozenset({'k'})) is False


def test_dispatch_shape_sig_builds_canonical_order():
    assert recomp._dispatch_shape_sig('void', frozenset({'a', 'j', 'k'})) == \
        'void(uint8_k,uint8_j,uint8_a)'
    assert recomp._dispatch_shape_sig('uint8', frozenset({'k', 'a'})) == \
        'uint8(uint8_k,uint8_a)'
    assert recomp._dispatch_shape_sig('void', frozenset()) == 'void()'


def test_compute_dispatch_table_shapes_unions_handler_liveins():
    # One table with three handlers, union live-in X+Y+A.
    # Table shape = {k, j, a}; all handlers share it.
    cfg = recomp.Config()
    cfg.bank = 0
    cfg.dispatch_tables = [{0x8000, 0x9000, 0xA000}]
    cfg._dispatch_handler_livein = {
        0x8000: {'X'},
        0x9000: {'X', 'Y'},
        0xA000: {'X', 'A'},
    }
    cfg.sigs = {}
    shapes = recomp._compute_dispatch_table_shapes(cfg)
    expected = frozenset({'k', 'j', 'a'})
    assert shapes[0x8000] == expected
    assert shapes[0x9000] == expected
    assert shapes[0xA000] == expected


def test_compute_dispatch_table_shapes_unions_across_tables():
    # A handler shared across two tables gets the wider union.
    cfg = recomp.Config()
    cfg.bank = 0
    shared = 0x9000
    cfg.dispatch_tables = [
        {0x8000, shared},         # needs k only
        {shared, 0xA000},         # needs k, j (table 2's Y-reading handler)
    ]
    cfg._dispatch_handler_livein = {
        0x8000: {'X'},
        shared: {'X'},
        0xA000: {'X', 'Y'},
    }
    cfg.sigs = {}
    shapes = recomp._compute_dispatch_table_shapes(cfg)
    assert shapes[shared] == frozenset({'k', 'j'}), (
        f'shared handler must pick up the wider table\'s shape, '
        f'got {shapes[shared]!r}'
    )


def test_compute_dispatch_table_shapes_from_sigs_without_livein():
    # When _dispatch_handler_livein is empty (e.g. handlers never ran
    # through augment), the union pulls from cfg.sigs instead.
    cfg = recomp.Config()
    cfg.bank = 0
    cfg.dispatch_tables = [{0x8000, 0x9000}]
    cfg._dispatch_handler_livein = {}
    cfg.sigs = {
        0x8000: 'void(uint8_k)',
        0x9000: 'void(uint8_k,uint8_j)',
    }
    shapes = recomp._compute_dispatch_table_shapes(cfg)
    assert shapes[0x8000] == frozenset({'k', 'j'})
    assert shapes[0x9000] == frozenset({'k', 'j'})


def test_dispatch_shape_accepts_void_nullary():
    assert recomp._sig_matches_dispatch_shape('void()')


def test_dispatch_shape_accepts_void_uint8_k():
    assert recomp._sig_matches_dispatch_shape('void(uint8_k)')


def test_dispatch_shape_accepts_retay_uint8_k():
    # RetAY-returning dispatch handlers are safe: the FuncU8 cast at
    # the dispatch call site calls the function and discards the
    # struct return at the ABI level (returned via register, caller
    # ignores). Direct JSR callers still see the RetAY and consume it.
    assert recomp._sig_matches_dispatch_shape('RetAY(uint8_k)')


def test_dispatch_shape_accepts_rety_nullary():
    # FuncV-cast functions can also carry a RetY return safely.
    assert recomp._sig_matches_dispatch_shape('RetY()')


def test_dispatch_shape_rejects_extra_a_param():
    # `void(uint8_k, uint8_a)` — the dispatch cast can't pass the
    # second arg. This is the shape that segfaulted SprStatus01_Init's
    # table entry `SprXXX_Generic_Init_StandardSpritesInit` when the
    # augment pass incorrectly widened it.
    assert not recomp._sig_matches_dispatch_shape('void(uint8_k, uint8_a)')


def test_dispatch_shape_rejects_extra_j_param():
    assert not recomp._sig_matches_dispatch_shape('void(uint8_k, uint8_j)')


def test_dispatch_shape_rejects_single_non_k_param():
    # A handler declared to take only `a` (not `k`) can't be dispatched
    # through a FuncU8 table either: the dispatch cast passes `k`,
    # which would be silently assigned to the callee's `a` param. That
    # would typecheck but misbehaves semantically.
    assert not recomp._sig_matches_dispatch_shape('void(uint8_a)')


def test_dispatch_shape_treats_none_as_safe():
    # An unresolved sig is safe — the augment pass will fill it in
    # with a dispatch-compatible shape.
    assert recomp._sig_matches_dispatch_shape(None)
