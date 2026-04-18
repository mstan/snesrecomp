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
