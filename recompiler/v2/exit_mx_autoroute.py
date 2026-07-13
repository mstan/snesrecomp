"""snesrecomp.recompiler.v2.exit_mx_autoroute

Per-variant function exit-(M, X) inference for EVERY cfg `func` entry,
including non-leaf functions that call other cfg-declared subroutines.

The cfg `exit_mx_at <addr> <m> <x>` directive declares that the callee
at `<addr>` exits with (M, X) = (m, x). The v2 decoder consults this
when emitting JSR/JSL fall-through edges so the caller resumes decoding
with the right operand widths after the call. Without an entry, the
decoder assumes (M, X) are preserved — wrong whenever the callee runs
an internal SEP/REP that never restores before RTS/RTL. SMW's
`$00:F465` is the canonical case (`SEP #$20` first, no restore; root
cause of "Mario dies on slope" 2026-05-03).

History:

  - 2026-05-03 (reverted): unconstrained iterative fixpoint over every
    (addr, m, x) tuple. Regressed `GraphicsDecompress` into an infinite
    loop — intermediate exit-(M, X) values polluted the analyzer
    along unreachable paths.

  - 2026-05-13 → 2026-05-15: pivoted to leaf-only auto-detection.
    Functions with any internal JSR/JSL were skipped; their exit
    state required hand-written cfg directives. This covered ~31
    SMW sites cleanly but left non-leaf SEP/RTS dispatchers
    (`PlayerState00_00F9C9`, `BufferScrollingTiles_Layer1_Init`,
    etc.) needing manual hints. Three hints accreted across three
    sessions — same shape every time — exposing this as an unfinished
    class.

  - 2026-05-16 (this): drop the leaf-only restriction. The decoder's
    PHP/PLP-bracketed M/X tracking (snesrecomp 73e3d26) closed the
    soundness gap that broke the 2026-05-03 fixpoint. With that gap
    closed, single-direction iterative inference is sound:

      * Information flow is monotonic — every committed
        (target_pc24, em, ex) → (exit_m, exit_x) is never revised.
      * Each pass walks every cfg func and tries the variants not
        yet recorded. Decode uses the current `callee_exit_mx`.
      * Ambiguous exits (`analyze_function_exit_mx` returns None)
        are skipped, never papered over.
      * Bounded at 8 iterations defensively; SMW typical converges
        in 2-3.

Per-variant recording: each `(target_pc24, em, ex) → (exit_m, exit_x)`
is recorded individually in `cfg.exit_mx_at_per_variant`. The legacy
`cfg.exit_mx_at` 4-tuple broadcast is now reserved for hand-written
cfg directives (which are treated as authoritative — seeded first into
`callee_exit_mx` so they win over auto-detection).

Why per-variant is mandatory: many functions have variant-dependent
exits. A leaf with `REP #$20 ; RTS` always exits m=0 but preserves
entry X, so per-(entry_x) recording is the only correct model.
Broadcasting the cfg-default's exit to all four variants poisons
non-default callers — the original Bug C visual cascade was caused
by exactly this misanalysis.

Mutates each `BankCfg.exit_mx_at_per_variant` list in place. The
v2_regen.py `callee_exit_mx` builder consumes per-variant entries
directly, no broadcasting.

Public API:
    detect_and_route(parsed, rom) -> List[FixRecord]
"""

from __future__ import annotations

import sys
from dataclasses import dataclass
from pathlib import Path
from typing import List, Set, Tuple

_THIS_DIR = Path(__file__).resolve().parent
_RECOMPILER_DIR = _THIS_DIR.parent
for p in (str(_THIS_DIR), str(_RECOMPILER_DIR)):
    if p not in sys.path:
        sys.path.insert(0, p)

from v2.decoder import (  # noqa: E402
    decode_function, analyze_function_exit_mx, clear_decode_cache,
)


@dataclass(frozen=True)
class FixRecord:
    """One leaf function whose exit (M, X) state differs from entry."""
    bank: int
    addr16: int             # 16-bit local PC of the leaf function
    fn_name: str
    entry_m: int
    entry_x: int
    exit_m: int
    exit_x: int

    @property
    def addr_24(self) -> int:
        return (self.bank << 16) | (self.addr16 & 0xFFFF)


_MX_COMBOS: List[Tuple[int, int]] = [(0, 0), (0, 1), (1, 0), (1, 1)]


def _decode_variant_exit(rom: bytes, bank: int, addr16: int,
                         em: int, ex: int, end,
                         callee_exit_mx,
                         dispatch_helpers=None):
    """Decode (bank, addr16) with entry (em, ex) and the provided
    callee_exit_mx for any JSR/JSL fall-through. Returns (exit_m,
    exit_x) if the body decoded cleanly with an unambiguous exit
    state, else None.

    Unlike the old `_decode_leaf_exit`, this does NOT skip functions
    that contain JSR/JSL. Soundness comes from:

      - PHP/PLP-bracketed M/X tracking in the decoder (snesrecomp
        73e3d26) — `PLP` correctly restores M/X to whatever PHP saved,
        so functions with internal SEP/REP wrapped in PHP/PLP get the
        right post-RTS state.
      - `callee_exit_mx` passed in: prior auto-router iterations have
        already determined exits for shorter-call-chain callees;
        this function inherits their state at JSR/JSL fall-through.
      - `analyze_function_exit_mx` returns None for ambiguous exits
        (multiple RTS paths disagree); we skip those.
      - `dispatch_helpers` passed in: lets the decoder recognise SMW's
        `JSL <helper>; <data table>` pattern as a dispatch terminator
        instead of misdecoding the table bytes as garbage code. With
        the helper map, dispatch JSLs land in the decode graph with
        `dispatch_entries` set and no fall-through successors, and
        `analyze_function_exit_mx` propagates exits through them.

    The 2026-05-03 iterative-fixpoint attempt regressed
    `GraphicsDecompress` because intermediate (m, x) values polluted
    the analyzer's downstream decisions. The mitigation here:

      - Single-direction information flow: callee_exit_mx only ever
        grows; we never revise a previously-committed exit.
      - Iteration bound: max 5 passes (typical SMW is 2-3).
      - Ambiguity gate: only commit when both m and x are determinate.
    """
    try:
        graph = decode_function(rom, bank, addr16,
                                entry_m=em, entry_x=ex, end=end,
                                dispatch_helpers=dispatch_helpers,
                                callee_exit_mx=callee_exit_mx)
    except Exception:
        return None
    if not graph.insns:
        return None
    exit_m, exit_x = analyze_function_exit_mx(graph, callee_exit_mx)
    if exit_m is None or exit_x is None:
        return None
    return (exit_m & 1, exit_x & 1)


def detect_and_route(parsed, rom: bytes,
                     dispatch_helpers=None) -> List[FixRecord]:
    """Auto-detect per-variant function exit-(M, X) state.

    Algorithm:

    1. Seed `callee_exit_mx` from hand-written cfg `exit_mx_at`
       directives (broadcast 4-tuple → all 4 entry variants). These
       seeds are IMMUTABLE for the duration of the analysis — the
       autoroute never overrides a hand-written hint.

    2. Iterate up to `_MAX_ITERS` passes. Each pass RE-DERIVES every
       cfg `func` entry's exit at every entry variant (skipping
       seeded keys) using the current `callee_exit_mx`. If the
       derived exit differs from the previously stored one, update
       `callee_exit_mx` and mark the pass dirty.

    3. Stop when a pass changes nothing (fixpoint), or after
       `_MAX_ITERS` passes (defensive bound).

    4. After fixpoint, emit `cfg.exit_mx_at_per_variant` entries for
       every (target, em, ex) whose final exit differs from entry
       (the "mutating" set).

    Per-variant emission: each `(target, entry_m, entry_x) → (exit_m,
    exit_x)` is recorded individually. The legacy `cfg.exit_mx_at`
    4-tuple broadcast is reserved for hand-written cfg directives;
    auto-detected exits go to `cfg.exit_mx_at_per_variant`.

    Soundness — why re-derivation is now correct:

      - PHP/PLP bracketing is modeled by the decoder (snesrecomp
        73e3d26); the previous PHP/PLP gap that biased the
        2026-05-03 fixpoint is closed.
      - Ambiguous exits (analyze_function_exit_mx returns None) are
        skipped, not papered over with a guess.
      - Each pass's analysis is a function of the current
        callee_exit_mx (no other mutable state), so any oscillation
        would have to come from a non-monotone update rule — which
        we don't have.

    Why the previous "commit-once, never revise" rule was buggy:

      Consider `WallRun` (`INX INX REP #$20 ; ... JSR $F465 ; RTS`)
      and `F465` (`SEP #$20 ; ... RTS`). WallRun at M1X1 entry hits
      the REP, then JSRs $F465 at em=0, ex=1. The correct chain:
      F465's exit at (0, 1) is (1, 1) (SEP #$20 forces m=1), so
      WallRun's post-JSR state is (1, 1) and its RTS exit is (1, 1).
      But if pass 1 analyses WallRun BEFORE F465 is recorded, the
      decoder default-preserves the JSR fall-through state and
      computes WallRun's exit as (0, 1). The old "commit-once" rule
      then locked that wrong (0, 1) record in even after F465
      committed (1, 1) in pass 2. Downstream callers of WallRun_M1X1
      cascade through the wrong post-call state into phantom
      M0X1-tracked code regions that runtime never reaches at m=0
      (the 2026-05-16 `RunPlayerBlockCode_00F28C_M0X1` verifier trip
      was this cascade). Re-deriving per pass fixes the order-
      dependence.

    Hand-written cfg `exit_mx_at` directives win at the same
    (target_pc24, em, ex) key — `seeded_keys` are skipped in every
    re-derivation pass and the per-variant emit step.

    Returns the list of `FixRecord` for the build report.
    """
    fixes: List[FixRecord] = []

    # Seed: cfg-declared 4-tuple exit_mx_at directives broadcast to
    # all 4 entry variants. Hand-written hints take precedence over
    # auto-detection — they are immutable for the rest of this run.
    callee_exit_mx: dict = {}
    seeded_keys: Set[Tuple[int, int, int]] = set()
    for bank, _cfg_path, cfg in parsed:
        for (b_id, addr16, m_val, x_val) in cfg.exit_mx_at:
            target_pc24 = ((b_id & 0xFF) << 16) | (addr16 & 0xFFFF)
            for em, ex in _MX_COMBOS:
                key = (target_pc24, em, ex)
                callee_exit_mx[key] = (m_val & 1, x_val & 1)
                seeded_keys.add(key)

    # Iterative fixpoint with re-derivation. Each pass walks every
    # cfg entry × every (em, ex); decodes under the current
    # callee_exit_mx; updates the entry if the derived exit differs
    # from what's stored. Stops when no entries change.
    _MAX_ITERS = 12
    for iter_n in range(_MAX_ITERS):
        # Treat each fixed-point iteration as an immutable semantic snapshot.
        # Decoder cache keys use strong object identity so lookup remains O(1);
        # never mutate a mapping that has already participated in a decode.
        clear_decode_cache()
        analysis_snapshot = dict(callee_exit_mx)
        next_callee_exit_mx = dict(callee_exit_mx)
        dirty = False
        for bank, _cfg_path, cfg in parsed:
            for entry in cfg.entries:
                if not entry.name:
                    continue
                addr16 = entry.start & 0xFFFF
                target_pc24 = (bank << 16) | addr16

                for em, ex in _MX_COMBOS:
                    key = (target_pc24, em, ex)
                    if key in seeded_keys:
                        continue  # hand-written hint is authoritative

                    exit_pair = _decode_variant_exit(
                        rom, bank, addr16, em, ex, entry.end,
                        analysis_snapshot,
                        dispatch_helpers=dispatch_helpers)
                    if exit_pair is None:
                        # Body decoded but exit is ambiguous, or
                        # decode failed. Drop any prior record so
                        # callers fall back to default-preserve
                        # rather than relying on a stale stored
                        # value. (Rare in practice — analyzer is
                        # deterministic given inputs, so this only
                        # fires when an upstream change introduces
                        # ambiguity.)
                        if key in next_callee_exit_mx:
                            del next_callee_exit_mx[key]
                            dirty = True
                        continue

                    prev = next_callee_exit_mx.get(key)
                    if prev != exit_pair:
                        next_callee_exit_mx[key] = exit_pair
                        dirty = True

        callee_exit_mx = next_callee_exit_mx
        if not dirty:
            break

    # Emit per-variant records AFTER fixpoint. Only `mutating`
    # entries (exit != entry) get cfg records; non-mutating exits
    # rely on the decoder's default-preserve. Seeded keys are
    # already covered by the hand-written `exit_mx_at` broadcast.
    for bank, _cfg_path, cfg in parsed:
        for entry in cfg.entries:
            if not entry.name:
                continue
            addr16 = entry.start & 0xFFFF
            target_pc24 = (bank << 16) | addr16
            for em, ex in _MX_COMBOS:
                key = (target_pc24, em, ex)
                if key in seeded_keys:
                    continue
                pair = callee_exit_mx.get(key)
                if pair is None:
                    continue
                exit_m, exit_x = pair
                if exit_m != em or exit_x != ex:
                    cfg.exit_mx_at_per_variant.append(
                        (bank, addr16, em, ex, exit_m, exit_x))
                    fixes.append(FixRecord(
                        bank=bank, addr16=addr16,
                        fn_name=entry.name,
                        entry_m=em, entry_x=ex,
                        exit_m=exit_m, exit_x=exit_x,
                    ))

    return fixes


def format_fix_summary(fixes: List[FixRecord]) -> str:
    """Build a human-readable summary block."""
    if not fixes:
        return "  no leaf-function exit-(M, X) mutations detected"
    lines = [f"  detected {len(fixes)} leaf-function exit-(M, X) mutation(s); auto-routed:"]
    for fx in fixes:
        lines.append(
            f"    ${fx.bank:02X}:{fx.addr16:04X} {fx.fn_name!r} "
            f"entry M={fx.entry_m} X={fx.entry_x} "
            f"-> exit M={fx.exit_m} X={fx.exit_x}"
        )
    return "\n".join(lines)
