"""snesrecomp.recompiler.v2.tail_call_autoroute

Auto-detect cfg `tail_call:<pc>` directive sites.

SMW (and similar games) sometimes give two callable entry points to a
single shared body — entry A is a short preamble that does setup and
then deliberately falls through into entry B, which is the rest of the
routine. Concretely:

    BufferFileSelectText:                 ; $00:9D38
        LDX #$CB                          ; 2 bytes (A2 CB)
        STZ $05                           ; 2 bytes (64 05)
    BufferFileSelectText_Entry3:          ; $00:9D3C — falls through
        ; ... shared body ...

Without intervention, the v2 decoder cuts A at the cfg `end:` boundary,
then emits an unresolvable-goto trap when control would fall through
into B's PC. cfg directive `tail_call:<pc>` declares B's PC as a
sibling tail-call target; emit_function then synthesises an explicit
tail call instead.

The directive is opt-in. This module auto-detects every site that
matches the byte-level invariant — A.end == B.start, A's last decoded
instruction does NOT terminate control flow (no RTS/RTL/RTI/JMP/JML/
BRA/BRL), and A's last instruction's PC + length == B.start. The opt-
in directive is preserved when present; the heuristic only synthesises
`tail_call_pc16` when the BankEntry has none set.

Runs as a decoder pre-pass in v2_regen.py after cfg load but before
codegen, like wrapper_autoroute.

Public API:
    detect_and_route(parsed, rom) -> List[FixRecord]
"""

from __future__ import annotations

import sys
from dataclasses import dataclass
from pathlib import Path
from typing import List

_THIS_DIR = Path(__file__).resolve().parent
_RECOMPILER_DIR = _THIS_DIR.parent
for p in (str(_THIS_DIR), str(_RECOMPILER_DIR)):
    if p not in sys.path:
        sys.path.insert(0, p)

from v2.decoder import decode_function  # noqa: E402


# 65816 mnemonics that unconditionally transfer control away (terminate
# the linear instruction stream of a function). If A's last instruction
# is one of these, A doesn't fall through; no tail call is possible.
# - RTS/RTL/RTI: return
# - JMP/JML:     unconditional jump (any addressing mode)
# - BRA/BRL:     unconditional branch (any size)
# Note: JSR/JSL are NOT terminal — they save a return address and
# resume after the call.
_TERMINAL_MNEMS = frozenset({'RTS', 'RTL', 'RTI', 'JMP', 'JML', 'BRA', 'BRL'})


@dataclass(frozen=True)
class FixRecord:
    """One detected tail-call fallthrough that was auto-routed."""
    bank: int
    src_pc16: int            # PC of the function with the fallthrough
    src_name: str
    dst_pc16: int            # PC of the sibling function (B)
    dst_name: str
    last_insn_pc16: int      # PC of A's last decoded instruction
    last_insn_mnem: str

    @property
    def src_addr_24(self) -> int:
        return (self.bank << 16) | (self.src_pc16 & 0xFFFF)

    @property
    def dst_addr_24(self) -> int:
        return (self.bank << 16) | (self.dst_pc16 & 0xFFFF)


def detect_and_route(parsed, rom: bytes) -> List[FixRecord]:
    """Detect cfg `func A end:<pc>` entries where <pc> is the start of
    another cfg `func B` AND A's last decoded instruction is a fall-
    through. Auto-set A.tail_call_pc16 in place.

    `parsed` is the v2_regen list of (bank, cfg_path, BankCfg). Mutates
    BankEntry.tail_call_pc16 on matched entries.

    Returns the list of applied fixes.
    """
    fixes: List[FixRecord] = []

    for bank, _cfg_path, cfg in parsed:
        # Build start_pc16 -> BankEntry index for fast B lookup.
        by_start = {
            (e.start & 0xFFFF): e
            for e in cfg.entries
            if e.name and e.start is not None
        }

        for entry in cfg.entries:
            # Skip entries without a name (synthetic), or already-
            # declared tail-calls, or no end: boundary to anchor against.
            if not entry.name:
                continue
            if entry.tail_call_pc16 is not None:
                continue
            if entry.end is None:
                continue
            end_pc = entry.end & 0xFFFF
            sibling = by_start.get(end_pc)
            if sibling is None or sibling is entry:
                continue

            # Decode A and inspect its last instruction.
            try:
                graph = decode_function(
                    rom, bank, entry.start & 0xFFFF,
                    entry_m=entry.entry_m,
                    entry_x=entry.entry_x,
                    end=entry.end,
                )
            except Exception:
                continue
            if not graph.insns:
                continue

            # The "last" instruction is the one with the highest PC
            # among decoded instructions. (graph.insns is keyed by
            # DecodeKey but we only care about PC ordering.)
            last_di = max(graph.insns.values(), key=lambda di: di.insn.addr)
            last_insn = last_di.insn
            last_pc16 = last_insn.addr & 0xFFFF

            # The fall-through invariant: last_pc + last_length == end_pc.
            # If equal, A naturally ends right at B.start with no
            # intervening bytes — the canonical "falls through" shape.
            if last_pc16 + last_insn.length != end_pc:
                continue

            if last_insn.mnem in _TERMINAL_MNEMS:
                continue

            # All invariants hold. Auto-route.
            entry.tail_call_pc16 = end_pc
            fixes.append(FixRecord(
                bank=bank,
                src_pc16=entry.start & 0xFFFF,
                src_name=entry.name,
                dst_pc16=end_pc,
                dst_name=sibling.name or f"_anon_{end_pc:04X}",
                last_insn_pc16=last_pc16,
                last_insn_mnem=last_insn.mnem,
            ))

    return fixes


def format_fix_summary(fixes: List[FixRecord]) -> str:
    """Build a human-readable summary block."""
    if not fixes:
        return "  no tail-call fallthrough sites detected"
    lines = [f"  detected {len(fixes)} tail-call fallthrough(s); auto-routed:"]
    for fx in fixes:
        lines.append(
            f"    ${fx.bank:02X}:{fx.src_pc16:04X} {fx.src_name!r} "
            f"--falls-through ({fx.last_insn_mnem} @ ${fx.last_insn_pc16:04X})--> "
            f"${fx.bank:02X}:{fx.dst_pc16:04X} {fx.dst_name!r}"
        )
    return "\n".join(lines)
