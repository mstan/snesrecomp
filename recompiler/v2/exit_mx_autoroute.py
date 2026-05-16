"""snesrecomp.recompiler.v2.exit_mx_autoroute

Auto-detect cfg `exit_mx_at` directive sites — leaf-function variant only.

The cfg `exit_mx_at <addr> <m> <x>` directive declares that the callee at
`<addr>` exits with (M, X) = (m, x). The v2 decoder consults this when
emitting JSR/JSL fall-through edges so the caller resumes decoding with
the right operand widths after the call. Without the directive the
decoder assumes (M, X) are preserved — wrong whenever the callee runs
an internal SEP/REP that never restores before RTS/RTL. SMW's
`$00:F465` is the canonical case (`SEP #$20` first, no restore; root
cause of "Mario dies on slope" 2026-05-03).

The directive is opt-in. An earlier (2026-05-03) attempt to auto-infer
it over EVERY decoded (addr, m, x) variant via fixpoint regressed
GraphicsDecompress into an infinite loop — the fixpoint produced
intermediate exit-(M,X) values that biased the analyzer along
unreachable paths. That attempt was reverted to opt-in.

This pass takes a DIFFERENT, narrower approach: it only auto-detects
LEAF functions (no JSR/JSL anywhere in the decoded body). Leaf
functions' exit (M, X) is determined purely by their own SEP/REP and
entry state — no callee dependency, no fixpoint required, no regression
risk from intermediate state. The trade-off: non-leaf functions whose
exits depend on their callees (like F461/F465 themselves, which call
into deeper routines) are still opt-in via cfg directive.

Detection has two passes per cfg `func`:

1. **cfg-declared entry mutates** — decode with the cfg-declared (M, X);
   if exit != entry, commit that exit. Original behavior since
   `14c8eea`.

2. **Multi-variant convergent (added 2026-05-14)** — when the
   cfg-declared entry's exit == entry (would skip under pass 1), scan
   the other three (M, X) combos. If every successful decode is a leaf
   and ALL decoded exits agree on a single (M, X) tuple AND at least
   one entry mutates, commit that exit. Closes the FileSelectColorMath-
   shape class: leafs whose SEP/REP forces (m, x) into a fixed value
   regardless of entry, but whose cfg-declared entry happens to land
   on the post-SEP/REP state. Per the audit in
   `tools/audit_leaf_exit_mx_variants.py`, 31 SMW sites fall in this
   class before the extension.

Mutates each `BankCfg.exit_mx_at` list in place with the inferred
tuples, so the existing builder at `v2_regen.py:342+` picks them up
when constructing `callee_exit_mx`.

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

from v2.decoder import decode_function, analyze_function_exit_mx  # noqa: E402


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


def _graph_has_call(graph) -> bool:
    """Return True if the decoded graph contains any JSR or JSL."""
    for di in graph.insns.values():
        if di.insn.mnem in ('JSR', 'JSL'):
            return True
    return False


_MX_COMBOS: List[Tuple[int, int]] = [(0, 0), (0, 1), (1, 0), (1, 1)]


def _decode_leaf_exit(rom: bytes, bank: int, addr16: int,
                      em: int, ex: int, end):
    """Decode (bank, addr16) with entry (em, ex). Returns
    (exit_m, exit_x) if the body decoded cleanly AND is a leaf with a
    determinable exit, else None."""
    try:
        graph = decode_function(rom, bank, addr16,
                                entry_m=em, entry_x=ex, end=end)
    except Exception:
        return None
    if not graph.insns:
        return None
    if _graph_has_call(graph):
        return None
    exit_m, exit_x = analyze_function_exit_mx(graph)
    if exit_m is None or exit_x is None:
        return None
    return (exit_m & 1, exit_x & 1)


def detect_and_route(parsed, rom: bytes) -> List[FixRecord]:
    """Auto-detect leaf-function exit-(M, X) state mutations.

    Two-pass per cfg `func` entry F:

    **Pass 1** — cfg-declared entry mutates:
      1. Decode F with its declared (entry_m, entry_x).
      2. Skip if non-leaf or exit is ambiguous.
      3. If exit != entry, commit that exit.

    **Pass 2** — multi-variant convergent (pass 1 skipped):
      4. Decode F under all four (M, X) combos. Skip if ANY decode is
         non-leaf, fails, or is ambiguous (conservative).
      5. If all four entries produce the same exit (M, X) AND at least
         one entry mutates, commit that exit.

    Sites already covered by a cfg `exit_mx_at` directive are skipped.
    Mutates the owning `BankCfg.exit_mx_at` list and returns the
    applied fixes.
    """
    fixes: List[FixRecord] = []

    declared: Set[Tuple[int, int]] = set()
    for bank, _cfg_path, cfg in parsed:
        for (b_id, addr16, _m, _x) in cfg.exit_mx_at:
            declared.add((b_id & 0xFF, addr16 & 0xFFFF))

    seen_keys: Set[Tuple[int, int]] = set()

    for bank, _cfg_path, cfg in parsed:
        for entry in cfg.entries:
            if not entry.name:
                continue
            addr16 = entry.start & 0xFFFF
            if (bank, addr16) in declared:
                continue  # cfg-declared wins
            if (bank, addr16) in seen_keys:
                continue

            em_in = entry.entry_m & 1
            ex_in = entry.entry_x & 1

            cfg_exit = _decode_leaf_exit(rom, bank, addr16,
                                         em_in, ex_in, entry.end)

            # Pass 1: cfg-declared entry mutates.
            if cfg_exit is not None:
                exit_m, exit_x = cfg_exit
                if exit_m != em_in or exit_x != ex_in:
                    seen_keys.add((bank, addr16))
                    cfg.exit_mx_at.append(
                        (bank, addr16, exit_m, exit_x))
                    fixes.append(FixRecord(
                        bank=bank, addr16=addr16, fn_name=entry.name,
                        entry_m=em_in, entry_x=ex_in,
                        exit_m=exit_m, exit_x=exit_x,
                    ))
                    continue

            # Pass 2: multi-variant convergent. The cfg-declared entry
            # either didn't mutate or didn't decode. Scan all four
            # combos — if every entry decodes as a leaf with a known
            # exit AND all four exits agree AND ≥1 entry mutates,
            # commit that exit.
            entry_exits: List[Tuple[int, int, int, int]] = []
            ok = True
            for em, ex in _MX_COMBOS:
                e = _decode_leaf_exit(rom, bank, addr16, em, ex,
                                      entry.end)
                if e is None:
                    ok = False
                    break
                entry_exits.append((em, ex, e[0], e[1]))
            if not ok:
                continue
            unique_exits = set((m, x) for (_, _, m, x) in entry_exits)
            if len(unique_exits) != 1:
                continue  # divergent — needs per-variant directive
            exit_m, exit_x = next(iter(unique_exits))
            any_mutates = any((em != m or ex != x)
                              for (em, ex, m, x) in entry_exits)
            if not any_mutates:
                continue  # all four entries are pass-through

            seen_keys.add((bank, addr16))
            cfg.exit_mx_at.append((bank, addr16, exit_m, exit_x))
            fixes.append(FixRecord(
                bank=bank, addr16=addr16, fn_name=entry.name,
                entry_m=em_in, entry_x=ex_in,
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
