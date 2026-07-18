"""Import DKC2 function structure from the H4v0c21 assembly disassembly.

The old DKC2 seed used an entries-only WLA symbol overlay containing direct
JSR/JSL targets.  Emitting those addresses as unbounded ``func`` declarations
gave the analyzer names, but not function ranges, and omitted the large set of
handlers reached through assembly ``dw``/``dl`` dispatch tables.

This importer combines three authoritative facts from the byte-exact assembly:

* the entries-only WLA overlay supplies direct-call targets;
* global labels whose first body statement is CPU code distinguish code labels
  from data labels;
* code labels referenced by ``dw``/``dl`` tables (plus ``*_entry`` stubs) are
  indirect control-flow entries.

Each imported function is bounded by the next imported entry in the same bank.
That is the same canonical contract used by the Zelda and Super Metroid decomp
importers.  Literal fall-through across a boundary remains supported by v2's
tail-call autorouter.

The output is deterministic and intentionally contains no ROM-derived bytes.
It consists only of names, PCs, and structural CFG metadata from the referenced
assembly address map. The validated H4 revision has no explicit license; its
assembly source and comments are not copied into the generated CFG files.
"""
from __future__ import annotations

import argparse
import re
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


SYM_RE = re.compile(
    r"^([0-9A-Fa-f]{2}):([0-9A-Fa-f]{4})\s+(\S+)\s*$"
)
DEBUG_RE = re.compile(
    r"^([0-9a-f]{2}):([0-9a-f]{4})\s+"
    r"([0-9A-Fa-f]{4}):([0-9A-Fa-f]{8})\s*$"
)
GLOBAL_LABEL_RE = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*):")
# Asar's `#Label:` form defines a global symbol without changing the active
# dot-local parent. H4 uses it for externally named state-table handlers.
NONSCOPING_GLOBAL_LABEL_RE = re.compile(
    r"^#([A-Za-z_][A-Za-z0-9_]*):")
LOCAL_LABEL_RE = re.compile(r"^(\.{1,2})([A-Za-z_][A-Za-z0-9_]*):?\s*$")
TABLE_RE = re.compile(r"^\s*(?:dw|dl)\s+(.+)$", re.IGNORECASE)
SYMBOL_TOKEN_RE = re.compile(
    r"(?<![A-Za-z0-9_])(\.{0,2}[A-Za-z_][A-Za-z0-9_]*)"
)
DATA_RE = re.compile(
    r"^(?:db|dw|dl|dd|incbin|fill|skip)\b", re.IGNORECASE
)

# Source statements that do not establish whether a pending label denotes code
# or data.  H4 uses these between a label and the first emitted statement.
NEUTRAL_RE = re.compile(
    r"^(?:if|elseif|else|endif|while|endwhile|assert|warnpc|check|"
    r"org|base|namespace|pushpc|pullpc|pad|padbyte|align|print)\b",
    re.IGNORECASE,
)

CPU_MNEMONICS = frozenset(
    "ADC AND ASL BCC BCS BEQ BIT BMI BNE BPL BRA BRK BRL BVC BVS CLC "
    "CLD CLI CLV CMP COP CPX CPY DEC DEX DEY EOR INC INX INY JMP JML "
    "JSR JSL LDA LDX LDY LSR MVN MVP NOP ORA PEA PEI PER PHA PHB PHD "
    "PHK PHP PHX PHY PLA PLB PLD PLP PLX PLY REP ROL ROR RTI RTL RTS "
    "SBC SEC SED SEI SEP STA STP STX STY STZ TAX TAY TCD TCS TDC TRB "
    "TSB TSC TSX TXA TXS TXY TYA TYX WAI WDM XBA XCE".split()
)


@dataclass(frozen=True)
class Entry:
    pc24: int
    name: str
    source: str


@dataclass(frozen=True)
class DispatchContract:
    bank: int
    site_pc16: int
    targets: tuple[int, ...]
    mode: str = "ptrtail"
    # Some dispatch ABIs enter a named table stub in a mode different from
    # DKC2's usual M0X0 function ABI.  These are cfg-root mode facts, separate
    # from ``targets`` (which names the actual post-dispatch instruction).
    entry_mx_overrides: tuple[tuple[int, int, int], ...] = ()


def collect_data_regions(
    full_symbols: Path, disasm_dir: Path
) -> list[tuple[int, int, int]]:
    """Recover exact assembled spans emitted by H4 data directives.

    Asar's debug rows give the start PC of every active emitted source line.
    A data statement therefore occupies the half-open span up to the next
    emitted row in the same bank. Adjacent statements are merged so the cfg
    stays compact. Conditional branches absent from the selected build have
    no debug row and cannot manufacture a region.
    """
    line_pc = _source_line_pc_maps(full_symbols, disasm_dir)
    spans: list[tuple[int, int, int]] = []
    for path in sorted(disasm_dir.glob("bank_*.asm")):
        rows: list[tuple[int, str]] = []
        for line_number, raw_line in enumerate(path.read_text(
            encoding="utf-8", errors="replace"
        ).splitlines(), 1):
            pc24 = line_pc.get((path, line_number))
            if pc24 is None:
                continue
            source = raw_line.split(";", 1)[0].strip()
            rows.append((pc24, _statement_kind(source) or "neutral"))
        for index, (pc24, kind) in enumerate(rows[:-1]):
            if kind != "data":
                continue
            next_pc24 = rows[index + 1][0]
            bank = (pc24 >> 16) & 0xFF
            start = pc24 & 0xFFFF
            if ((next_pc24 >> 16) & 0xFF) != bank:
                continue
            end = next_pc24 & 0xFFFF
            if end > start:
                spans.append((bank, start, end))

    merged: list[tuple[int, int, int]] = []
    for bank, start, end in sorted(set(spans)):
        if merged and merged[-1][0] == bank and start <= merged[-1][2]:
            old_bank, old_start, old_end = merged[-1]
            merged[-1] = (old_bank, old_start, max(old_end, end))
        else:
            merged.append((bank, start, end))
    return merged


@dataclass
class _SourceScope:
    """Resolve Asar's dot-local labels to flattened `.sym` names."""

    global_name: str | None = None
    local_name: str | None = None

    def define(self, dots: str, name: str) -> str | None:
        if not dots:
            self.global_name = name
            self.local_name = None
            return name
        if self.global_name is None:
            return None
        if len(dots) == 1:
            self.local_name = name
            return f"{self.global_name}_{name}"
        if self.local_name is None:
            return None
        return f"{self.global_name}_{self.local_name}_{name}"

    def resolve(self, token: str) -> str | None:
        dots = len(token) - len(token.lstrip("."))
        name = token[dots:]
        if dots == 0:
            return name
        if self.global_name is None:
            return None
        if dots == 1:
            return f"{self.global_name}_{name}"
        if self.local_name is None:
            return None
        return f"{self.global_name}_{self.local_name}_{name}"


def parse_wla_symbols(path: Path) -> list[Entry]:
    """Read a WLA symbol file, preserving aliases at the same address."""
    out: list[Entry] = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = SYM_RE.match(line)
        if not match:
            continue
        pc24 = (int(match.group(1), 16) << 16) | int(match.group(2), 16)
        out.append(Entry(pc24, match.group(3), "direct"))
    return out


def _source_line_pc_maps(
    full_symbols: Path, disasm_dir: Path
) -> dict[tuple[Path, int], int]:
    """Map exact H4 source lines to assembled PCs via Asar debug symbols.

    H4's human ``;$BBxxxx`` comments can drift after conditional assembly.
    The lowercase rows in Asar's verified `.sym` instead encode
    ``bank:pc file-id:source-line`` and are exact for the selected build.
    """
    records: dict[tuple[int, str], list[tuple[int, int]]] = defaultdict(list)
    for raw in full_symbols.read_text(
        encoding="utf-8", errors="replace"
    ).splitlines():
        match = DEBUG_RE.match(raw)
        if not match:
            continue
        bank = int(match.group(1), 16)
        pc16 = int(match.group(2), 16)
        file_id = match.group(3).upper()
        line_number = int(match.group(4), 16)
        records[(bank, file_id)].append((line_number, pc16))

    result: dict[tuple[Path, int], int] = {}
    for path in sorted(disasm_dir.glob("bank_*.asm")):
        bank_match = re.search(r"bank_([0-9A-Fa-f]{2})\.asm$", path.name)
        if not bank_match:
            continue
        bank = int(bank_match.group(1), 16)
        line_count = len(path.read_text(
            encoding="utf-8", errors="replace"
        ).splitlines())
        candidates = []
        for (record_bank, file_id), rows in records.items():
            if record_bank != bank:
                continue
            usable = [(line, pc) for line, pc in rows
                      if 1 <= line <= line_count]
            if usable:
                candidates.append((len(usable), file_id, usable))
        if not candidates:
            continue
        _count, _file_id, rows = max(candidates, key=lambda item: item[:2])
        for line, pc16 in rows:
            result[(path, line)] = (bank << 16) | pc16
    return result


def collect_pointer_contracts(
    full_symbols: Path, disasm_dir: Path
) -> tuple[list[DispatchContract], list[Entry]]:
    """Recover bounded runtime-pointer targets declared by H4 source.

    DKC2 writes exactly two local continuations to
    ``sprite_return_address`` and every sprite ends via
    ``JML [sprite_return_address]``.  This is a finite decomp contract, not a
    heuristic: enumerate the assignments and attach that target universe to
    each exact indirect site using Asar's line map.
    """
    full_entries = parse_wla_symbols(full_symbols)
    full_by_name: dict[str, set[int]] = defaultdict(set)
    names_by_pc: dict[int, list[str]] = defaultdict(list)
    for entry in full_entries:
        full_by_name[entry.name].add(entry.pc24)
        names_by_pc[entry.pc24].append(entry.name)

    line_pc = _source_line_pc_maps(full_symbols, disasm_dir)
    targets: set[int] = set()
    site_pcs: set[int] = set()
    load_re = re.compile(
        r"^LDA(?:\.[bwl])?\s+#(?:<:)?"
        r"(\.{0,2}[A-Za-z_][A-Za-z0-9_]*)\b", re.IGNORECASE)
    store_re = re.compile(
        r"^STA(?:\.[bwl])?\s+sprite_return_address\b", re.IGNORECASE)
    site_re = re.compile(
        r"^JML\s+\[sprite_return_address\]", re.IGNORECASE)

    for path in sorted(disasm_dir.glob("bank_*.asm")):
        scope = _SourceScope()
        pending_target: int | None = None
        for line_number, raw_line in enumerate(path.read_text(
            encoding="utf-8", errors="replace"
        ).splitlines(), 1):
            source = raw_line.split(";", 1)[0].strip()
            global_label = GLOBAL_LABEL_RE.match(source)
            local_label = LOCAL_LABEL_RE.match(source)
            if global_label:
                scope.define("", global_label.group(1))
                continue
            if local_label:
                scope.define(local_label.group(1), local_label.group(2))
                continue
            load = load_re.match(source)
            if load:
                resolved = scope.resolve(load.group(1))
                addresses = full_by_name.get(resolved or "", set())
                pending_target = next(iter(addresses)) if len(addresses) == 1 else None
                continue
            if store_re.match(source):
                if pending_target is not None:
                    targets.add(pending_target)
                pending_target = None
                continue
            if site_re.match(source):
                pc24 = line_pc.get((path, line_number))
                if pc24 is not None:
                    site_pcs.add(pc24)
                pending_target = None
                continue
            if _statement_kind(source) == "code":
                pending_target = None

    sorted_targets = tuple(sorted(targets))
    contracts = [
        DispatchContract((pc >> 16) & 0xFF, pc & 0xFFFF, sorted_targets)
        for pc in sorted(site_pcs)
        if sorted_targets
    ]
    target_entries = []
    for pc24 in sorted_targets:
        friendly = sorted(
            name for name in names_by_pc.get(pc24, ())
            if not re.fullmatch(r"[0-9A-Fa-f]{4}:[0-9A-Fa-f]{8}", name)
        )
        name = friendly[0] if friendly else f"pointer_target_{pc24:06X}"
        target_entries.append(Entry(pc24, name, "indirect"))
    return contracts, target_entries


def collect_animation_callback_contracts(
    full_symbols: Path, disasm_dir: Path
) -> tuple[list[DispatchContract], list[Entry]]:
    """Import DKC2 animation bytecode's finite callback ABI.

    Commands $81/$83/$84/$8F/$90 embed a 16-bit bank-$B9 code pointer as
    their first word. H4 spells those records symbolically as
    ``db !animation_command_XX : dw callback, ...``. The five interpreter
    sites in ``process_sprite_animation`` all execute through ``temp_26``.
    Commands $81/$8F/$90 and the continuous $84 callback push an explicit
    PEA return frame before the jump, so those sites are pointer calls.
    Command $83 deliberately does not: its callback must tail-transfer back
    into the animation engine (or otherwise consume the animation engine's
    existing return context). Treating $83 as a pointer call invents a
    two-byte return frame and leaks those bytes on the guest stack.
    """
    full_entries = parse_wla_symbols(full_symbols)
    full_by_name: dict[str, set[int]] = defaultdict(set)
    names_by_pc: dict[int, list[str]] = defaultdict(list)
    for entry in full_entries:
        full_by_name[entry.name].add(entry.pc24)
        names_by_pc[entry.pc24].append(entry.name)

    command_re = re.compile(
        r"\bdb\s+!animation_command_(81|83|84|8F|90)\s*:\s*"
        r"dw\s+(\.{0,2}[A-Za-z_][A-Za-z0-9_]*)", re.IGNORECASE)
    targets_by_command: dict[str, set[int]] = defaultdict(set)
    for path in sorted(disasm_dir.glob("bank_*.asm")):
        scope = _SourceScope()
        for raw_line in path.read_text(
            encoding="utf-8", errors="replace"
        ).splitlines():
            source = raw_line.split(";", 1)[0].strip()
            global_label = GLOBAL_LABEL_RE.match(source)
            local_label = LOCAL_LABEL_RE.match(source)
            if global_label:
                scope.define("", global_label.group(1))
                continue
            if local_label:
                scope.define(local_label.group(1), local_label.group(2))
                continue
            match = command_re.search(source)
            if not match:
                continue
            resolved = scope.resolve(match.group(2))
            addresses = full_by_name.get(resolved or "", set())
            if len(addresses) != 1:
                continue
            pc24 = next(iter(addresses))
            # The bytecode stores a 16-bit pointer and the executor forces PB
            # to $B9 before jumping. Reject any symbol that violates that ABI.
            if (pc24 >> 16) == 0xB9:
                targets_by_command[match.group(1).upper()].add(pc24)

    line_pc = _source_line_pc_maps(full_symbols, disasm_dir)
    contracts: list[DispatchContract] = []
    for path in sorted(disasm_dir.glob("bank_*.asm")):
        current_global: str | None = None
        saw_continuous_pointer_load = False
        for line_number, raw_line in enumerate(path.read_text(
            encoding="utf-8", errors="replace"
        ).splitlines(), 1):
            source = raw_line.split(";", 1)[0].strip()
            global_label = GLOBAL_LABEL_RE.match(source)
            if global_label:
                current_global = global_label.group(1)
                continue
            if re.match(
                r"^LDA(?:\.[bwl])?\s+sprite\.animation_routine,x\b",
                source, re.IGNORECASE
            ):
                saw_continuous_pointer_load = True
            if not re.match(r"^JMP\s+\(temp_26\)", source, re.IGNORECASE):
                continue
            command = None
            match = re.fullmatch(r"animation_command_(81|83|8F|90)",
                                 current_global or "", re.IGNORECASE)
            if match:
                command = match.group(1).upper()
            elif saw_continuous_pointer_load:
                command = "84"
            pc24 = line_pc.get((path, line_number))
            targets = tuple(sorted(targets_by_command.get(command or "", ())))
            if pc24 is not None and targets:
                contracts.append(DispatchContract(
                    (pc24 >> 16) & 0xFF, pc24 & 0xFFFF,
                    targets,
                    mode="ptrtail" if command == "83" else "ptrcall"))
            saw_continuous_pointer_load = False

    all_targets = sorted(set().union(*targets_by_command.values())) \
        if targets_by_command else []
    target_entries: list[Entry] = []
    for pc24 in all_targets:
        friendly = sorted(
            name for name in names_by_pc.get(pc24, ())
            if not re.fullmatch(r"[0-9A-Fa-f]{4}:[0-9A-Fa-f]{8}", name)
        )
        name = friendly[0] if friendly else f"animation_callback_{pc24:06X}"
        target_entries.append(Entry(pc24, name, "indirect"))
    return contracts, target_entries


def collect_sprite_state_contracts(
    full_symbols: Path, disasm_dir: Path
) -> tuple[list[DispatchContract], list[Entry], list[int]]:
    """Import the inline sprite-state tables consumed by the B3/BE helpers.

    Every call to ``sprite_state_handler_B3`` or ``sprite_state_handler_BE``
    places a contiguous ``dw`` state table immediately after its return
    address.  The helper removes that return address, adds the live sprite
    state index, and finishes with ``JMP ($0000,x)``.  H4 spells every table
    element symbolically, so their union is an exact finite target contract
    for the helper's one runtime-pointer site.
    """
    full_entries = parse_wla_symbols(full_symbols)
    full_by_name: dict[str, set[int]] = defaultdict(set)
    names_by_pc: dict[int, list[str]] = defaultdict(list)
    for entry in full_entries:
        full_by_name[entry.name].add(entry.pc24)
        names_by_pc[entry.pc24].append(entry.name)

    helper_re = re.compile(
        r"^(JSR|JMP)\s+(sprite_state_handler_(B3|BE))\b",
        re.IGNORECASE,
    )
    line_pc = _source_line_pc_maps(full_symbols, disasm_dir)
    targets_by_helper: dict[str, set[int]] = defaultdict(set)
    terminal_jsr_sites: set[int] = set()
    for path in sorted(disasm_dir.glob("bank_*.asm")):
        scope = _SourceScope()
        collecting_helper: str | None = None
        for line_number, raw_line in enumerate(path.read_text(
            encoding="utf-8", errors="replace"
        ).splitlines(), 1):
            source = raw_line.split(";", 1)[0].strip()
            global_label = GLOBAL_LABEL_RE.match(source)
            local_label = LOCAL_LABEL_RE.match(source)
            if global_label:
                scope.define("", global_label.group(1))
                continue
            if local_label:
                scope.define(local_label.group(1), local_label.group(2))
                continue

            helper = helper_re.match(source)
            if helper:
                collecting_helper = helper.group(2).lower()
                if helper.group(1).upper() == "JSR":
                    pc24 = line_pc.get((path, line_number))
                    if pc24 is not None:
                        terminal_jsr_sites.add(pc24)
                continue

            table = TABLE_RE.match(source)
            if collecting_helper is not None and table:
                expected_bank = int(collecting_helper.rsplit("_", 1)[1], 16)
                for token in SYMBOL_TOKEN_RE.findall(table.group(1)):
                    resolved = scope.resolve(token)
                    addresses = full_by_name.get(resolved or "", set())
                    if len(addresses) != 1:
                        continue
                    pc24 = next(iter(addresses))
                    if (pc24 >> 16) == expected_bank:
                        targets_by_helper[collecting_helper].add(pc24)
                continue

            if source and _statement_kind(source) is not None:
                collecting_helper = None

    site_by_helper: dict[str, int] = {}
    for path in sorted(disasm_dir.glob("bank_*.asm")):
        current_global: str | None = None
        for line_number, raw_line in enumerate(path.read_text(
            encoding="utf-8", errors="replace"
        ).splitlines(), 1):
            source = raw_line.split(";", 1)[0].strip()
            global_label = GLOBAL_LABEL_RE.match(source)
            if global_label:
                current_global = global_label.group(1).lower()
                continue
            if (current_global in ("sprite_state_handler_b3",
                                   "sprite_state_handler_be")
                    and re.match(r"^JMP\s+\(\$0000,x\)", source,
                                 re.IGNORECASE)):
                pc24 = line_pc.get((path, line_number))
                if pc24 is not None:
                    site_by_helper[current_global] = pc24

    contracts: list[DispatchContract] = []
    all_targets: set[int] = set()
    for helper, targets in sorted(targets_by_helper.items()):
        site = site_by_helper.get(helper)
        sorted_targets = tuple(sorted(targets))
        if site is None or not sorted_targets:
            continue
        contracts.append(DispatchContract(
            (site >> 16) & 0xFF, site & 0xFFFF, sorted_targets,
            mode="ptrtail_popcall"))
        all_targets.update(sorted_targets)

    target_entries: list[Entry] = []
    for pc24 in sorted(all_targets):
        friendly = sorted(
            name for name in names_by_pc.get(pc24, ())
            if not re.fullmatch(r"[0-9A-Fa-f]{4}:[0-9A-Fa-f]{8}", name)
        )
        name = friendly[0] if friendly else f"sprite_state_{pc24:06X}"
        target_entries.append(Entry(pc24, name, "indirect"))
    return contracts, target_entries, sorted(terminal_jsr_sites)


def collect_collision_pointer_contracts(
    full_symbols: Path, disasm_dir: Path
) -> tuple[list[DispatchContract], list[Entry]]:
    """Import the finite long-call ABI behind the sprite collision pointer.

    Player-clipping setup assigns one of two symbolic routines to
    ``sprite_collision_routine_address``.  Sprite code invokes that live
    24-bit pointer after pushing an RTL continuation, so every
    ``JML [sprite_collision_routine_address]`` is a pointer call whose target
    universe is exactly the set of symbolic assignments in H4.
    """
    full_entries = parse_wla_symbols(full_symbols)
    full_by_name: dict[str, set[int]] = defaultdict(set)
    names_by_pc: dict[int, list[str]] = defaultdict(list)
    for entry in full_entries:
        full_by_name[entry.name].add(entry.pc24)
        names_by_pc[entry.pc24].append(entry.name)

    line_pc = _source_line_pc_maps(full_symbols, disasm_dir)
    load_re = re.compile(
        r"^LD[AY](?:\.[bwl])?\s+#(?:<:)?"
        r"(\.{0,2}[A-Za-z_][A-Za-z0-9_]*)\b", re.IGNORECASE)
    store_re = re.compile(
        r"^ST[AY](?:\.[bwl])?\s+sprite_collision_routine_address\b",
        re.IGNORECASE)
    site_re = re.compile(
        r"^JML\s+\[sprite_collision_routine_address\]", re.IGNORECASE)
    targets: set[int] = set()
    sites: set[int] = set()

    for path in sorted(disasm_dir.glob("bank_*.asm")):
        scope = _SourceScope()
        pending_target: int | None = None
        for line_number, raw_line in enumerate(path.read_text(
            encoding="utf-8", errors="replace"
        ).splitlines(), 1):
            source = raw_line.split(";", 1)[0].strip()
            global_label = GLOBAL_LABEL_RE.match(source)
            local_label = LOCAL_LABEL_RE.match(source)
            if global_label:
                scope.define("", global_label.group(1))
                continue
            if local_label:
                scope.define(local_label.group(1), local_label.group(2))
                continue
            load = load_re.match(source)
            if load:
                resolved = scope.resolve(load.group(1))
                addresses = full_by_name.get(resolved or "", set())
                pending_target = next(iter(addresses)) \
                    if len(addresses) == 1 else None
                continue
            if store_re.match(source):
                if pending_target is not None:
                    targets.add(pending_target)
                pending_target = None
                continue
            if site_re.match(source):
                pc24 = line_pc.get((path, line_number))
                if pc24 is not None:
                    sites.add(pc24)
                pending_target = None
                continue
            if source and _statement_kind(source) == "code":
                pending_target = None

    sorted_targets = tuple(sorted(targets))
    contracts = [
        DispatchContract((site >> 16) & 0xFF, site & 0xFFFF,
                         sorted_targets, mode="ptrcall")
        for site in sorted(sites)
        if sorted_targets
    ]
    target_entries: list[Entry] = []
    for pc24 in sorted_targets:
        friendly = sorted(
            name for name in names_by_pc.get(pc24, ())
            if not re.fullmatch(r"[0-9A-Fa-f]{4}:[0-9A-Fa-f]{8}", name)
        )
        name = friendly[0] if friendly else f"collision_target_{pc24:06X}"
        target_entries.append(Entry(pc24, name, "indirect"))
    return contracts, target_entries


def collect_indexed_record_dispatch_contracts(
    full_symbols: Path, disasm_dir: Path
) -> tuple[list[DispatchContract], list[Entry]]:
    """Import H4's complete stride-four handler/flags dispatch tables.

    Sprite type values are byte offsets into records written as
    ``dw main_handler, time_stop_flags``.  Both normal and time-stop sprite
    loops load that type into X and execute ``JMP (sprite_main_table,x)``.
    Kong state values use the identical layout in ``kong_state_table``.
    Only the first field is executable; the second is data consumed by the
    parallel flags lookup, so requiring a two-field row with a numeric second
    field keeps each target universe exact.
    """
    full_entries = parse_wla_symbols(full_symbols)
    full_by_name: dict[str, set[int]] = defaultdict(set)
    names_by_pc: dict[int, list[str]] = defaultdict(list)
    for entry in full_entries:
        full_by_name[entry.name].add(entry.pc24)
        names_by_pc[entry.pc24].append(entry.name)

    line_pc = _source_line_pc_maps(full_symbols, disasm_dir)
    numeric_re = re.compile(r"^\s*(?:\$[0-9A-Fa-f]+|%[01]+|[0-9]+)\s*$")
    record_tables = {"sprite_main_table", "kong_state_table"}
    site_re = re.compile(
        r"^JMP\s+\((sprite_main_table|kong_state_table)\s*,\s*x\)",
        re.IGNORECASE)
    targets_by_table: dict[str, set[int]] = defaultdict(set)
    sites_by_table: dict[str, set[int]] = defaultdict(set)

    for path in sorted(disasm_dir.glob("bank_*.asm")):
        scope = _SourceScope()
        in_table: str | None = None
        for line_number, raw_line in enumerate(path.read_text(
            encoding="utf-8", errors="replace"
        ).splitlines(), 1):
            source = raw_line.split(";", 1)[0].strip()
            global_label = GLOBAL_LABEL_RE.match(source)
            local_label = LOCAL_LABEL_RE.match(source)
            if global_label:
                name = global_label.group(1)
                scope.define("", name)
                in_table = name if name in record_tables else None
                continue
            if local_label:
                scope.define(local_label.group(1), local_label.group(2))
                continue

            if in_table:
                if re.match(
                    r"^(?:if|elseif|else|endif)\b", source, re.IGNORECASE
                ):
                    # Asar conditionals select version-specific rows but do not
                    # terminate the surrounding dispatch table.  Import the
                    # union of their targets; the ROM fixes which row is live.
                    continue
                if re.match(r"^%offset\s*\(", source, re.IGNORECASE):
                    # Asar layout assertion for the parallel flags table;
                    # it is metadata, not the end of sprite_main_table.
                    continue
                table = TABLE_RE.match(source)
                if table:
                    fields = [part.strip() for part in table.group(1).split(",")]
                    if len(fields) == 2 and numeric_re.match(fields[1]):
                        token = fields[0]
                        if re.fullmatch(
                            r"\.{0,2}[A-Za-z_][A-Za-z0-9_]*", token
                        ):
                            resolved = scope.resolve(token)
                            addresses = full_by_name.get(resolved or "", set())
                            if len(addresses) == 1:
                                targets_by_table[in_table].add(
                                    next(iter(addresses)))
                    continue
                if source and _statement_kind(source) != "neutral":
                    in_table = None

            site = site_re.match(source)
            if site:
                pc24 = line_pc.get((path, line_number))
                if pc24 is not None:
                    sites_by_table[site.group(1).lower()].add(pc24)

    contracts: list[DispatchContract] = []
    all_targets: set[int] = set()
    for table_name in sorted(record_tables):
        sorted_targets = tuple(sorted(targets_by_table[table_name]))
        all_targets.update(sorted_targets)
        contracts.extend(
            DispatchContract((site >> 16) & 0xFF, site & 0xFFFF,
                             sorted_targets, mode="ptrtail")
            for site in sorted(sites_by_table[table_name])
            if sorted_targets
        )
    target_entries: list[Entry] = []
    for pc24 in sorted(all_targets):
        friendly = sorted(
            name for name in names_by_pc.get(pc24, ())
            if not re.fullmatch(r"[0-9A-Fa-f]{4}:[0-9A-Fa-f]{8}", name)
        )
        name = friendly[0] if friendly else f"record_handler_{pc24:06X}"
        target_entries.append(Entry(pc24, name, "indirect"))
    return contracts, target_entries


def collect_symbolic_indexed_dispatch_contracts(
    full_symbols: Path, disasm_dir: Path
) -> tuple[list[DispatchContract], list[Entry]]:
    """Import ordinary symbolic ``JMP (table,x)`` pointer tables.

    H4 expresses many sprite/boss state machines as a local or global label,
    one or more contiguous ``dw`` rows, and an absolute-indexed indirect jump
    through that label.  The byte decoder cannot safely infer the table length
    from ROM alone; if it treats the words following the jump as code, a later
    coincidental BRK/COP poisons the whole routine and every shared caller.

    Only uniquely resolved symbols whose source labels begin CPU code are
    accepted as targets, and a 16-bit table entry must remain in the jump's
    program bank.  Runtime RAM pointers and mixed data records therefore stay
    outside this contract.  Specialized importers may override a site when
    their table ABI selects only one field of a wider record.
    """
    full_entries = parse_wla_symbols(full_symbols)
    full_by_name: dict[str, set[int]] = defaultdict(set)
    names_by_pc: dict[int, list[str]] = defaultdict(list)
    for entry in full_entries:
        full_by_name[entry.name].add(entry.pc24)
        names_by_pc[entry.pc24].append(entry.name)

    label_kind, _table_refs = scan_disassembly(disasm_dir)
    line_pc = _source_line_pc_maps(full_symbols, disasm_dir)
    table_targets: dict[str, set[int]] = defaultdict(set)

    # First pass: bind each labeled contiguous DW run to its symbolic code
    # targets. Multiple aliases immediately before the first row share it.
    for path in sorted(disasm_dir.glob("bank_*.asm")):
        scope = _SourceScope()
        pending_labels: list[str] = []
        active_tables: list[str] = []
        for raw_line in path.read_text(
            encoding="utf-8", errors="replace"
        ).splitlines():
            source = raw_line.split(";", 1)[0].strip()
            global_label = GLOBAL_LABEL_RE.match(source)
            nonscoping_global = NONSCOPING_GLOBAL_LABEL_RE.match(source)
            local_label = LOCAL_LABEL_RE.match(source)
            if global_label:
                resolved = scope.define("", global_label.group(1))
                pending_labels = [resolved] if resolved else []
                active_tables = []
                continue
            if nonscoping_global:
                pending_labels = [nonscoping_global.group(1)]
                active_tables = []
                continue
            if local_label:
                resolved = scope.define(
                    local_label.group(1), local_label.group(2))
                if resolved:
                    pending_labels.append(resolved)
                active_tables = []
                continue

            table = TABLE_RE.match(source)
            if table and source.lower().startswith("dw"):
                if pending_labels:
                    active_tables = list(pending_labels)
                    pending_labels.clear()
                if not active_tables:
                    continue
                for token in SYMBOL_TOKEN_RE.findall(table.group(1)):
                    resolved = scope.resolve(token)
                    if not resolved or label_kind.get(resolved) != "code":
                        continue
                    addresses = full_by_name.get(resolved, set())
                    if len(addresses) != 1:
                        continue
                    target = next(iter(addresses))
                    for table_name in active_tables:
                        table_targets[table_name].add(target)
                continue

            if source and _statement_kind(source) is not None:
                pending_labels.clear()
                active_tables = []

    site_re = re.compile(
        r"^JMP\s+\(\s*(\.{0,2}[A-Za-z_][A-Za-z0-9_]*)\s*,\s*x\s*\)",
        re.IGNORECASE,
    )
    contracts: list[DispatchContract] = []
    all_targets: set[int] = set()
    for path in sorted(disasm_dir.glob("bank_*.asm")):
        scope = _SourceScope()
        for line_number, raw_line in enumerate(path.read_text(
            encoding="utf-8", errors="replace"
        ).splitlines(), 1):
            source = raw_line.split(";", 1)[0].strip()
            global_label = GLOBAL_LABEL_RE.match(source)
            nonscoping_global = NONSCOPING_GLOBAL_LABEL_RE.match(source)
            local_label = LOCAL_LABEL_RE.match(source)
            if global_label:
                scope.define("", global_label.group(1))
                continue
            if nonscoping_global:
                continue
            if local_label:
                scope.define(local_label.group(1), local_label.group(2))
                continue
            match = site_re.match(source)
            if not match:
                continue
            table_name = scope.resolve(match.group(1))
            pc24 = line_pc.get((path, line_number))
            targets = tuple(sorted(table_targets.get(table_name or "", ())))
            if pc24 is None or not targets:
                continue
            bank = (pc24 >> 16) & 0xFF
            targets = tuple(target for target in targets
                            if ((target >> 16) & 0xFF) == bank)
            if not targets:
                continue
            contracts.append(DispatchContract(
                bank, pc24 & 0xFFFF, targets, mode="ptrtail"))
            all_targets.update(targets)

    target_entries: list[Entry] = []
    for pc24 in sorted(all_targets):
        friendly = sorted(
            name for name in names_by_pc.get(pc24, ())
            if not re.fullmatch(r"[0-9A-Fa-f]{4}:[0-9A-Fa-f]{8}", name)
        )
        name = friendly[0] if friendly else f"indexed_target_{pc24:06X}"
        target_entries.append(Entry(pc24, name, "indirect"))
    return contracts, target_entries


def collect_terrain_dispatch_contracts(
    full_symbols: Path, disasm_dir: Path
) -> tuple[list[DispatchContract], list[Entry]]:
    """Import the finite level-terrain routine pointer dispatch.

    Level setup loads ``$17B2`` from H4's symbolic ``DATA_B5BC00`` table;
    two level-specific setup paths overwrite it with symbolic immediates.
    ``get_sprite_terrain_info`` then tail-dispatches through ``JMP ($17B2)``.
    Those terrain routines in turn load ``$00AA`` from ``DATA_B5CA58`` and
    tail-dispatch through it.  Both tables therefore form exact, finite target
    universes directly described by the decomp.

    This contract matters disproportionately: the terrain helper is below the
    common sprite-movement dispatcher, so leaving its single indirect edge
    unresolved makes a large, very hot caller chain ineligible for AOT.
    """
    full_entries = parse_wla_symbols(full_symbols)
    full_by_name: dict[str, set[int]] = defaultdict(set)
    names_by_pc: dict[int, list[str]] = defaultdict(list)
    for entry in full_entries:
        full_by_name[entry.name].add(entry.pc24)
        names_by_pc[entry.pc24].append(entry.name)

    line_pc = _source_line_pc_maps(full_symbols, disasm_dir)
    table_pointer = {
        "DATA_B5BC00": "$17B2",
        "DATA_B5CA58": "$00AA",
    }
    targets_by_table: dict[str, set[int]] = defaultdict(set)
    sites_by_pointer: dict[str, set[int]] = defaultdict(set)
    active_table: str | None = None
    pending_target: int | None = None
    immediate_re = re.compile(
        r"^LDA(?:\.[bwl])?\s+#(?:<:)?"
        r"(\.{0,2}[A-Za-z_][A-Za-z0-9_]*)\b", re.IGNORECASE)
    store_re = re.compile(r"^STA(?:\.[bwl])?\s+\$17B2\b", re.IGNORECASE)
    site_re = re.compile(r"^JMP\s+\((\$[0-9A-Fa-f]{2,4})\)", re.IGNORECASE)

    for path in sorted(disasm_dir.glob("bank_*.asm")):
        scope = _SourceScope()
        active_table = None
        pending_target = None
        for line_number, raw_line in enumerate(path.read_text(
            encoding="utf-8", errors="replace"
        ).splitlines(), 1):
            source = raw_line.split(";", 1)[0].strip()
            global_label = GLOBAL_LABEL_RE.match(source)
            local_label = LOCAL_LABEL_RE.match(source)
            if global_label:
                name = global_label.group(1)
                scope.define("", name)
                active_table = name if name in table_pointer else None
                pending_target = None
                continue
            if local_label:
                scope.define(local_label.group(1), local_label.group(2))
                continue

            if active_table is not None:
                table = TABLE_RE.match(source)
                if table:
                    for token in SYMBOL_TOKEN_RE.findall(table.group(1)):
                        resolved = scope.resolve(token)
                        addresses = full_by_name.get(resolved or "", set())
                        if len(addresses) == 1:
                            targets_by_table[active_table].add(
                                next(iter(addresses)))
                    continue
                if source and _statement_kind(source) != "neutral":
                    active_table = None

            immediate = immediate_re.match(source)
            if immediate:
                resolved = scope.resolve(immediate.group(1))
                addresses = full_by_name.get(resolved or "", set())
                pending_target = next(iter(addresses)) \
                    if len(addresses) == 1 else None
                continue
            if store_re.match(source):
                if pending_target is not None:
                    targets_by_table["DATA_B5BC00"].add(pending_target)
                pending_target = None
                continue
            site = site_re.match(source)
            if site:
                pc24 = line_pc.get((path, line_number))
                if pc24 is not None:
                    pointer = site.group(1).upper()
                    if pointer == "$AA":
                        pointer = "$00AA"
                    sites_by_pointer[pointer].add(pc24)
                pending_target = None
                continue
            if source and _statement_kind(source) == "code":
                pending_target = None

    contracts: list[DispatchContract] = []
    all_targets: set[int] = set()
    for table_name, pointer in table_pointer.items():
        sorted_targets = tuple(sorted(targets_by_table[table_name]))
        all_targets.update(sorted_targets)
        contracts.extend(
            DispatchContract((site >> 16) & 0xFF, site & 0xFFFF,
                             sorted_targets, mode="ptrtail")
            for site in sorted(sites_by_pointer[pointer])
            if sorted_targets
        )
    target_entries: list[Entry] = []
    for pc24 in sorted(all_targets):
        friendly = sorted(
            name for name in names_by_pc.get(pc24, ())
            if not re.fullmatch(r"[0-9A-Fa-f]{4}:[0-9A-Fa-f]{8}", name)
        )
        name = friendly[0] if friendly else f"terrain_target_{pc24:06X}"
        target_entries.append(Entry(pc24, name, "indirect"))
    return contracts, target_entries


def collect_interaction_callback_contracts(
    full_symbols: Path, disasm_dir: Path
) -> tuple[list[DispatchContract], list[Entry]]:
    """Import the finite callback used by interactions 0D/0F/10/2B.

    H4 names the shared player handler ``player_interaction_0D_0F_10_2B``.
    Sprite setup stores a symbolic low word in ``interaction_RAM_0A8A`` and
    the matching ``symbol>>16`` bank in ``interaction_RAM_0A8C``.  The handler
    copies that pair to DP $32 and executes ``JML [$0032]``.  Requiring both
    halves of each symbolic assignment excludes the other interaction types
    that reuse $0A8A/$0A8C as ordinary data.
    """
    full_entries = parse_wla_symbols(full_symbols)
    full_by_name: dict[str, set[int]] = defaultdict(set)
    names_by_pc: dict[int, list[str]] = defaultdict(list)
    for entry in full_entries:
        full_by_name[entry.name].add(entry.pc24)
        names_by_pc[entry.pc24].append(entry.name)

    line_pc = _source_line_pc_maps(full_symbols, disasm_dir)
    low_load_re = re.compile(
        r"^LDA(?:\.[bwl])?\s+#(?:<:)?"
        r"(\.{0,2}[A-Za-z_][A-Za-z0-9_]*)\s*$", re.IGNORECASE)
    high_load_re = re.compile(
        r"^LDA(?:\.[bwl])?\s+#(?:<:)?"
        r"(\.{0,2}[A-Za-z_][A-Za-z0-9_]*)>>16\s*$", re.IGNORECASE)
    low_store_re = re.compile(
        r"^STA(?:\.[bwl])?\s+interaction_RAM_0A8A\b", re.IGNORECASE)
    high_store_re = re.compile(
        r"^STA(?:\.[bwl])?\s+interaction_RAM_0A8C\b", re.IGNORECASE)
    site_re = re.compile(r"^JML\s+\[\$0032\]", re.IGNORECASE)
    targets: set[int] = set()
    sites: set[int] = set()

    for path in sorted(disasm_dir.glob("bank_*.asm")):
        scope = _SourceScope()
        loaded_low: tuple[str, int] | None = None
        stored_low: tuple[str, int] | None = None
        loaded_high_name: str | None = None
        for line_number, raw_line in enumerate(path.read_text(
            encoding="utf-8", errors="replace"
        ).splitlines(), 1):
            source = raw_line.split(";", 1)[0].strip()
            global_label = GLOBAL_LABEL_RE.match(source)
            local_label = LOCAL_LABEL_RE.match(source)
            if global_label:
                scope.define("", global_label.group(1))
                loaded_low = stored_low = None
                loaded_high_name = None
                continue
            if local_label:
                scope.define(local_label.group(1), local_label.group(2))
                continue

            high_load = high_load_re.match(source)
            if high_load:
                loaded_high_name = scope.resolve(high_load.group(1))
                loaded_low = None
                continue
            low_load = low_load_re.match(source)
            if low_load:
                resolved = scope.resolve(low_load.group(1))
                addresses = full_by_name.get(resolved or "", set())
                loaded_low = (resolved or "", next(iter(addresses))) \
                    if len(addresses) == 1 else None
                loaded_high_name = None
                continue
            if low_store_re.match(source):
                stored_low = loaded_low
                loaded_low = None
                continue
            if high_store_re.match(source):
                if (stored_low is not None and loaded_high_name is not None
                        and stored_low[0] == loaded_high_name):
                    targets.add(stored_low[1])
                stored_low = None
                loaded_high_name = None
                continue
            if (scope.global_name == "CODE_B8938A"
                    and site_re.match(source)):
                pc24 = line_pc.get((path, line_number))
                if pc24 is not None:
                    sites.add(pc24)
                continue
            if source and _statement_kind(source) == "code":
                loaded_low = None
                loaded_high_name = None

    sorted_targets = tuple(sorted(targets))
    contracts = [
        DispatchContract((site >> 16) & 0xFF, site & 0xFFFF,
                         sorted_targets, mode="ptrtail")
        for site in sorted(sites)
        if sorted_targets
    ]
    target_entries: list[Entry] = []
    for pc24 in sorted_targets:
        friendly = sorted(
            name for name in names_by_pc.get(pc24, ())
            if not re.fullmatch(r"[0-9A-Fa-f]{4}:[0-9A-Fa-f]{8}", name)
        )
        name = friendly[0] if friendly else f"interaction_callback_{pc24:06X}"
        target_entries.append(Entry(pc24, name, "indirect"))
    return contracts, target_entries


def collect_kong_cutscene_contracts(
    full_symbols: Path, disasm_dir: Path
) -> tuple[list[DispatchContract], list[Entry]]:
    """Import the timer/handler record dispatch in ``kong_cutscene_handler``.

    Each script command is written by H4 as ``dw timer, handler``.  The
    interpreter indexes a selected script, reads the timer at +0, then uses
    ``JMP ($0002,x)`` to tail-dispatch to the symbolic handler at +2.  The
    second fields of those records are therefore the exact target universe;
    the preceding table of script pointers is intentionally excluded because
    its rows contain only one value.
    """
    full_entries = parse_wla_symbols(full_symbols)
    full_by_name: dict[str, set[int]] = defaultdict(set)
    names_by_pc: dict[int, list[str]] = defaultdict(list)
    for entry in full_entries:
        full_by_name[entry.name].add(entry.pc24)
        names_by_pc[entry.pc24].append(entry.name)

    line_pc = _source_line_pc_maps(full_symbols, disasm_dir)
    numeric_re = re.compile(r"^\s*(?:\$[0-9A-Fa-f]+|%[01]+|[0-9]+)\s*$")
    site_re = re.compile(r"^JMP\s+\(\$0002,x\)", re.IGNORECASE)
    targets: set[int] = set()
    sites: set[int] = set()

    for path in sorted(disasm_dir.glob("bank_*.asm")):
        scope = _SourceScope()
        for line_number, raw_line in enumerate(path.read_text(
            encoding="utf-8", errors="replace"
        ).splitlines(), 1):
            source = raw_line.split(";", 1)[0].strip()
            global_label = GLOBAL_LABEL_RE.match(source)
            local_label = LOCAL_LABEL_RE.match(source)
            if global_label:
                scope.define("", global_label.group(1))
                continue
            if local_label:
                scope.define(local_label.group(1), local_label.group(2))
                continue
            # H4 places a second global label at the initialized handler body;
            # Asar consequently scopes both the script data and its handlers
            # under CODE_BBC174 even though callers enter at the preceding
            # friendly label kong_cutscene_handler.
            if scope.global_name != "CODE_BBC174":
                continue
            if site_re.match(source):
                pc24 = line_pc.get((path, line_number))
                if pc24 is not None:
                    sites.add(pc24)
                continue
            table = TABLE_RE.match(source)
            if not table:
                continue
            fields = [field.strip() for field in table.group(1).split(",")]
            if len(fields) != 2 or not numeric_re.match(fields[0]):
                continue
            tokens = SYMBOL_TOKEN_RE.findall(fields[1])
            if len(tokens) != 1:
                continue
            resolved = scope.resolve(tokens[0])
            addresses = full_by_name.get(resolved or "", set())
            if len(addresses) == 1:
                targets.add(next(iter(addresses)))

    sorted_targets = tuple(sorted(targets))
    contracts = [
        DispatchContract((site >> 16) & 0xFF, site & 0xFFFF,
                         sorted_targets, mode="ptrtail")
        for site in sorted(sites)
        if sorted_targets
    ]
    target_entries: list[Entry] = []
    for pc24 in sorted_targets:
        friendly = sorted(
            name for name in names_by_pc.get(pc24, ())
            if not re.fullmatch(r"[0-9A-Fa-f]{4}:[0-9A-Fa-f]{8}", name)
        )
        name = friendly[0] if friendly else f"kong_cutscene_target_{pc24:06X}"
        target_entries.append(Entry(pc24, name, "indirect"))
    return contracts, target_entries


def collect_rts_stack_dispatch_contracts(
    full_symbols: Path, disasm_dir: Path
) -> list[DispatchContract]:
    """Recover DKC2's finite PEI;RTS decompression command dispatches.

    The command words stored in DP are return addresses minus one.  ``PEI``
    pushes the word and the immediately following ``RTS`` lands on the JMP
    instruction in one of the two command-entry tables.  H4 exposes both the
    entry-table labels and the three dispatch instructions, while Asar debug
    rows provide their exact assembled PCs.

    This is emitted as an ``rtsstack`` contract rather than a ptrtail call:
    the handlers are an internal computed-goto component of the decompressor
    and share its saved DB/Y stack frame.
    """
    line_pc = _source_line_pc_maps(full_symbols, disasm_dir)
    full_by_name: dict[str, set[int]] = defaultdict(set)
    for entry in parse_wla_symbols(full_symbols):
        full_by_name[entry.name].add(entry.pc24)
    targets_by_set: dict[int, list[int]] = defaultdict(list)
    entry_pcs_by_set: dict[int, list[int]] = defaultdict(list)
    sites: list[tuple[int, int]] = []
    entry_re = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*_([12])_entry$")
    pei_re = re.compile(r"^PEI\s+\(\$([0-9A-Fa-f]{2})\)(?:\s|$)",
                        re.IGNORECASE)

    for path in sorted(disasm_dir.glob("bank_*.asm")):
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        current_set: int | None = None
        current_entry_pc24: int | None = None
        code_rows: list[tuple[int, str, int]] = []
        for line_number, raw_line in enumerate(lines, 1):
            source = raw_line.split(";", 1)[0].strip()
            if not source:
                continue
            global_label = GLOBAL_LABEL_RE.match(source)
            if global_label:
                match = entry_re.match(global_label.group(1))
                current_set = int(match.group(1)) if match else None
                addresses = full_by_name.get(global_label.group(1), ())
                current_entry_pc24 = (
                    next(iter(addresses)) if current_set is not None
                    and len(addresses) == 1 else None
                )
                continue

            pc24 = line_pc.get((path, line_number))
            if pc24 is None or _statement_kind(source) != "code":
                continue
            code_rows.append((line_number, source, pc24))
            # The actual RTS landing is the table stub's JMP instruction, not
            # necessarily its label/NOP address (set 2's first stub has no NOP).
            if current_set is not None and re.match(r"^JMP\b", source,
                                                    re.IGNORECASE):
                targets_by_set[current_set].append(pc24)
                if current_entry_pc24 is not None:
                    entry_pcs_by_set[current_set].append(current_entry_pc24)
                current_set = None
                current_entry_pc24 = None

        for index, (_line, source, pc24) in enumerate(code_rows[:-1]):
            match = pei_re.match(source)
            if not match:
                continue
            next_source = code_rows[index + 1][1]
            if not re.match(r"^RTS\b", next_source, re.IGNORECASE):
                continue
            dp = int(match.group(1), 16)
            command_set = {0x4E: 1, 0x4A: 2}.get(dp)
            if command_set is not None:
                sites.append((pc24, command_set))

    contracts: list[DispatchContract] = []
    for site, command_set in sorted(sites):
        targets = tuple(sorted(set(targets_by_set.get(command_set, ()))))
        if targets:
            # The three PEI/RTS sites execute after ``SEP #$20`` in the
            # decompressor, while X remains 16-bit.  The *_entry labels name
            # the one-byte runway immediately before (or exactly at) the JMP
            # landing.  Publishing those roots as M1X0 prevents an artificial
            # M0 decode from treating 8-bit operands as BRK/COP instructions.
            entry_mx_overrides = tuple(
                (pc24, 1, 0)
                for pc24 in sorted(set(entry_pcs_by_set.get(command_set, ())))
            )
            contracts.append(DispatchContract(
                bank=(site >> 16) & 0xFF,
                site_pc16=site & 0xFFFF,
                targets=targets,
                mode="rtsstack",
                entry_mx_overrides=entry_mx_overrides,
            ))
    return contracts


def _statement_kind(statement: str) -> str | None:
    """Return code/data for an emitted statement, or None for directives."""
    text = statement.strip()
    if not text or NEUTRAL_RE.match(text):
        return None
    if DATA_RE.match(text):
        return "data"
    if text.startswith("%"):
        return "code"
    word = re.match(r"^([A-Za-z]{3})(?:\.[bwl])?\b", text)
    if word and word.group(1).upper() in CPU_MNEMONICS:
        return "code"
    # Constant/struct/equate declarations are neither code nor data bodies.
    if "=" in text or text.startswith(("!", ".")):
        return None
    return None


def scan_disassembly(disasm_dir: Path) -> tuple[dict[str, str], Counter[str]]:
    """Classify global labels and collect symbolic table references."""
    label_kind: dict[str, str] = {}
    table_refs: Counter[str] = Counter()

    for path in sorted(disasm_dir.glob("bank_*.asm")):
        pending: list[str] = []
        scope = _SourceScope()
        for raw_line in path.read_text(
            encoding="utf-8", errors="replace"
        ).splitlines():
            source = raw_line.split(";", 1)[0].rstrip()
            stripped = source.strip()
            global_label = GLOBAL_LABEL_RE.match(stripped)
            nonscoping_global = NONSCOPING_GLOBAL_LABEL_RE.match(stripped)
            local_label = LOCAL_LABEL_RE.match(stripped)
            if global_label:
                resolved = scope.define("", global_label.group(1))
                if resolved:
                    pending.append(resolved)
                continue
            if nonscoping_global:
                pending.append(nonscoping_global.group(1))
                continue
            if local_label:
                resolved = scope.define(
                    local_label.group(1), local_label.group(2))
                if resolved:
                    pending.append(resolved)
                continue

            kind = _statement_kind(source)
            if pending and kind is not None:
                for name in pending:
                    label_kind.setdefault(name, kind)
                pending.clear()

            table = TABLE_RE.match(source)
            if table:
                for token in SYMBOL_TOKEN_RE.findall(table.group(1)):
                    resolved = scope.resolve(token)
                    if resolved:
                        table_refs[resolved] += 1

    return label_kind, table_refs


def collect_entries(
    direct_symbols: Path,
    full_symbols: Path,
    disasm_dir: Path,
    include_indirect: bool = True,
) -> list[Entry]:
    full_entries = parse_wla_symbols(full_symbols)
    full_by_name: dict[str, list[int]] = defaultdict(list)
    for entry in full_entries:
        full_by_name[entry.name].append(entry.pc24)

    # The historical entries-only overlay was harvested from H4's human
    # address comments.  Those comments drift after conditional assembly in a
    # number of regions, while the full Asar symbol file contains the exact
    # addresses for the selected ROM build.  Preserve the overlay's useful
    # declaration that a name is a direct entry, but resolve that name through
    # the authoritative full symbol map whenever it is unambiguous.
    full_by_local_suffix: dict[str, set[int]] = defaultdict(set)
    for entry in full_entries:
        parts = entry.name.split("_")
        for index in range(1, len(parts)):
            full_by_local_suffix["_".join(parts[index:])].add(entry.pc24)

    direct: list[Entry] = []
    for entry in parse_wla_symbols(direct_symbols):
        exact_addresses = set(full_by_name.get(entry.name, ()))
        if len(exact_addresses) != 1:
            # The entries overlay flattens scoped labels such as
            # ``parent_CODE_80D77A`` to ``CODE_80D77A``.  Recover those only
            # when the local suffix is unique in the selected build.
            exact_addresses = set(full_by_local_suffix.get(entry.name, ()))
        if len(exact_addresses) != 1:
            # A stale overlay entry absent from the selected Asar build often
            # belongs to a different regional/version conditional.  Keeping
            # its comment-derived address would manufacture a code entry in
            # unrelated bytes, so omit it.
            continue
        direct.append(Entry(
            next(iter(exact_addresses)), entry.name, entry.source))

    label_kind, table_refs = scan_disassembly(disasm_dir)
    code_banks = {entry.pc24 >> 16 for entry in direct}
    collected = list(direct)

    if include_indirect:
        candidates = set(table_refs)
        candidates.update(
            name for name, kind in label_kind.items()
            if kind == "code" and name.endswith("_entry")
        )
        for name in sorted(candidates):
            if label_kind.get(name) != "code":
                continue
            addresses = full_by_name.get(name, ())
            # A repeated unscoped name is ambiguous; do not guess which body a
            # table refers to.  H4's global handler names are unique in practice.
            if len(set(addresses)) != 1:
                continue
            pc24 = addresses[0]
            if (pc24 >> 16) not in code_banks:
                continue
            collected.append(Entry(pc24, name, "indirect"))

        _contracts, pointer_entries = collect_pointer_contracts(
            full_symbols, disasm_dir)
        collected.extend(pointer_entries)
        _animation_contracts, animation_entries = \
            collect_animation_callback_contracts(full_symbols, disasm_dir)
        collected.extend(animation_entries)
        _state_contracts, state_entries, _terminal_jsrs = collect_sprite_state_contracts(
            full_symbols, disasm_dir)
        collected.extend(state_entries)
        _collision_contracts, collision_entries = \
            collect_collision_pointer_contracts(full_symbols, disasm_dir)
        collected.extend(collision_entries)
        _record_contracts, record_entries = \
            collect_indexed_record_dispatch_contracts(
                full_symbols, disasm_dir)
        collected.extend(record_entries)
        _indexed_contracts, indexed_entries = \
            collect_symbolic_indexed_dispatch_contracts(
                full_symbols, disasm_dir)
        collected.extend(indexed_entries)
        _terrain_contracts, terrain_entries = \
            collect_terrain_dispatch_contracts(full_symbols, disasm_dir)
        collected.extend(terrain_entries)
        _interaction_contracts, interaction_entries = \
            collect_interaction_callback_contracts(full_symbols, disasm_dir)
        collected.extend(interaction_entries)
        _cutscene_contracts, cutscene_entries = \
            collect_kong_cutscene_contracts(full_symbols, disasm_dir)
        collected.extend(cutscene_entries)

    # Prefer a direct-call name at aliased PCs, then the lexicographically first
    # indirect name.  Ensure C identifiers remain unique across different PCs.
    by_pc: dict[int, list[Entry]] = defaultdict(list)
    for entry in collected:
        by_pc[entry.pc24].append(entry)

    used_names: dict[str, int] = {}
    result: list[Entry] = []
    for pc24 in sorted(by_pc):
        aliases = sorted(
            by_pc[pc24], key=lambda entry: (entry.source != "direct", entry.name)
        )
        chosen = aliases[0]
        name = re.sub(r"[^A-Za-z0-9_]", "_", chosen.name)
        if not name or name[0].isdigit():
            name = f"func_{name}"
        prior = used_names.get(name)
        if prior is not None and prior != pc24:
            name = f"{name}_{pc24:06X}"
        used_names[name] = pc24
        result.append(Entry(pc24, name, chosen.source))
    return result


def emit_cfg(entries: Iterable[Entry], output_dir: Path,
             dispatches: Iterable[DispatchContract] = (),
             data_regions: Iterable[tuple[int, int, int]] = (),
             terminal_jsrs: Iterable[int] = ()) -> None:
    by_bank: dict[int, list[Entry]] = defaultdict(list)
    for entry in entries:
        by_bank[(entry.pc24 >> 16) & 0xFF].append(entry)
    dispatches_by_bank: dict[int, list[DispatchContract]] = defaultdict(list)
    for dispatch in dispatches:
        dispatches_by_bank[dispatch.bank & 0xFF].append(dispatch)
    data_by_bank: dict[int, list[tuple[int, int]]] = defaultdict(list)
    for bank, start, end in data_regions:
        data_by_bank[bank & 0xFF].append((start, end))
    terminal_jsrs_by_bank: dict[int, set[int]] = defaultdict(set)
    for pc24 in terminal_jsrs:
        terminal_jsrs_by_bank[(pc24 >> 16) & 0xFF].add(pc24 & 0xFFFF)
    entry_modes: dict[int, set[tuple[int, int]]] = defaultdict(set)
    for dispatch in dispatches:
        for pc24, m_flag, x_flag in dispatch.entry_mx_overrides:
            entry_modes[pc24 & 0xFFFFFF].add((m_flag & 1, x_flag & 1))
    conflicts = {
        pc24: modes for pc24, modes in entry_modes.items() if len(modes) > 1
    }
    if conflicts:
        details = ", ".join(
            f"${pc24:06X}={sorted(modes)}"
            for pc24, modes in sorted(conflicts.items())
        )
        raise ValueError(f"conflicting inferred entry M/X modes: {details}")

    output_dir.mkdir(parents=True, exist_ok=True)
    for stale in output_dir.glob("bank*.cfg"):
        stale.unlink()

    # Architectural vectors execute through the bank-$00 HiROM mirror.  Keep
    # that bootstrap separate from H4's bank-$80 source labels so vector entry
    # modes come from the ROM header and missing bodies retain the LLE fallback.
    _write_text_lf(output_dir / "bank00.cfg", """\
# DKC2 architectural bootstrap; generated by ingest_dkc2_disasm.py.
bank = 0
auto_vectors
tier_down_stubs
""")

    for bank in sorted(by_bank):
        items = sorted(by_bank[bank], key=lambda entry: entry.pc24)
        direct_count = sum(entry.source == "direct" for entry in items)
        indirect_count = len(items) - direct_count
        lines = [
            f"bank = 0x{bank:02X}",
            "",
            "# Auto-generated by tools/ingest_dkc2_disasm.py.",
            "# Source: H4v0c21 byte-exact assembly + WLA symbols.",
            f"# {len(items)} bounded entries: {direct_count} direct, "
            f"{indirect_count} indirect.",
        ]
        for start, end in sorted(data_by_bank.get(bank, ())):
            lines.append(
                f"data_region {bank:02X} {start:04X} {end:04X}")
        for dispatch in sorted(
            dispatches_by_bank.get(bank, ()), key=lambda item: item.site_pc16
        ):
            targets = ",".join(f"{target:06X}" for target in dispatch.targets)
            lines.append(
                f"indirect_dispatch {dispatch.site_pc16:04X} "
                f"{len(dispatch.targets)} {dispatch.mode} targets:{targets}"
            )
        for site_pc16 in sorted(terminal_jsrs_by_bank.get(bank, ())):
            lines.append(f"terminal_jsr {site_pc16:04X}")
        for index, entry in enumerate(items):
            end = (
                items[index + 1].pc24 & 0xFFFF
                if index + 1 < len(items)
                else 0x10000
            )
            modes = entry_modes.get(entry.pc24 & 0xFFFFFF)
            entry_m, entry_x = next(iter(modes)) if modes else (0, 0)
            lines.append(
                f"func {entry.name} {entry.pc24 & 0xFFFF:04X} "
                f"end:{end:04X} entry_mx:{entry_m},{entry_x}"
            )
        _write_text_lf(
            output_dir / f"bank{bank:02x}.cfg", "\n".join(lines) + "\n")


def _write_text_lf(path: Path, text: str) -> None:
    """Write deterministic LF metadata even when the importer runs on Windows."""
    with path.open("w", encoding="utf-8", newline="\n") as stream:
        stream.write(text)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--disasm", type=Path, required=True)
    parser.add_argument("--entries-sym", type=Path, required=True)
    parser.add_argument("--full-sym", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--direct-only", action="store_true",
        help="emit bounded direct-call entries without table targets",
    )
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    for path in (args.disasm, args.entries_sym, args.full_sym):
        if not path.exists():
            print(f"missing input: {path}", file=sys.stderr)
            return 1

    entries = collect_entries(
        args.entries_sym,
        args.full_sym,
        args.disasm,
        include_indirect=not args.direct_only,
    )
    dispatches, _pointer_entries = collect_pointer_contracts(
        args.full_sym, args.disasm)
    animation_dispatches, _animation_entries = \
        collect_animation_callback_contracts(args.full_sym, args.disasm)
    dispatches.extend(animation_dispatches)
    state_dispatches, _state_entries, terminal_jsrs = collect_sprite_state_contracts(
        args.full_sym, args.disasm)
    dispatches.extend(state_dispatches)
    collision_dispatches, _collision_entries = \
        collect_collision_pointer_contracts(args.full_sym, args.disasm)
    dispatches.extend(collision_dispatches)
    record_dispatches, _record_entries = \
        collect_indexed_record_dispatch_contracts(args.full_sym, args.disasm)
    dispatches.extend(record_dispatches)
    indexed_dispatches, _indexed_entries = \
        collect_symbolic_indexed_dispatch_contracts(
            args.full_sym, args.disasm)
    # A specialized importer understands record stride/field semantics better
    # than the generic symbolic-DW collector. Preserve the first contract for
    # any already modeled site and add only genuinely new indexed tables.
    existing_sites = {(item.bank, item.site_pc16) for item in dispatches}
    dispatches.extend(
        item for item in indexed_dispatches
        if (item.bank, item.site_pc16) not in existing_sites)
    terrain_dispatches, _terrain_entries = \
        collect_terrain_dispatch_contracts(args.full_sym, args.disasm)
    dispatches.extend(terrain_dispatches)
    interaction_dispatches, _interaction_entries = \
        collect_interaction_callback_contracts(args.full_sym, args.disasm)
    dispatches.extend(interaction_dispatches)
    cutscene_dispatches, _cutscene_entries = \
        collect_kong_cutscene_contracts(args.full_sym, args.disasm)
    dispatches.extend(cutscene_dispatches)
    dispatches.extend(collect_rts_stack_dispatch_contracts(
        args.full_sym, args.disasm))
    data_regions = collect_data_regions(args.full_sym, args.disasm)
    histogram = Counter(entry.pc24 >> 16 for entry in entries)
    print(
        f"harvested {len(entries)} unique entries: "
        + ", ".join(f"${bank:02X}:{count}" for bank, count in sorted(histogram.items()))
    )
    print(f"harvested {len(dispatches)} bounded runtime-pointer site(s)")
    print(f"harvested {len(terminal_jsrs)} terminal inline-table JSR site(s)")
    print(f"harvested {len(data_regions)} exact data region(s)")
    if not args.dry_run:
        emit_cfg(entries, args.output, dispatches, data_regions, terminal_jsrs)
        print(f"wrote bounded cfg to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
