"""snesrecomp.recompiler.v2.cfg_loader

Parse a v1-format bank cfg file into a v2 `BankCfg` dataclass that
`emit_bank` can consume.

v2 IGNORES the v1 ABI-fiction directives:
- `sig:` — every v2 function is `void f(CpuState *cpu)`.
- `ret_y` / `carry_ret` — return value is never materialised; flags
  and registers live in the CpuState struct.
- `y_after:` / `x_after:` — call-site index increments are dead with
  the new ABI (caller and callee both see cpu->X / cpu->Y).
- `restores_x:` / DP-aliased struct params (e.g. `CollInfo_*ci`) —
  same, dead.

v2 KEEPS:
- `bank = NN`
- `includes = ...` (used to extend the default emit_bank header).
- `func <name> <hex_pc> [end:<hex_end>]` — the entry list. Anything
  on the line after the address is parsed for `end:` and otherwise
  discarded.
- `name <hex_addr> <name>` — friendly-naming for cross-bank labels.
  v2 emits these as `void NAME(CpuState *cpu);` forward declarations
  in funcs.h (Phase 6e/f). For now we just retain them.
- `exclude_range <start> <end>` — data region carved out of decode.
- `data_region <bank> <start> <end>` — same idea, cross-bank.
- `verbatim_start` / `verbatim_end` — hand-written C inlined at the
  top of the bank file (e.g. dispatch tables that aren't recompiler
  output).

Public API:
    load_bank_cfg(path: str) -> BankCfg
"""

import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Optional, Tuple

import sys

_THIS_DIR = Path(__file__).resolve().parent
_RECOMPILER_DIR = _THIS_DIR.parent
for p in (str(_THIS_DIR), str(_RECOMPILER_DIR)):
    if p not in sys.path:
        sys.path.insert(0, p)

from v2.emit_bank import BankEntry  # noqa: E402


@dataclass
class NameDecl:
    """A `name <addr> <friendly>` line. Cross-bank label / friendly
    name used by funcs.h forward decls; not an entry to emit here."""
    addr_24: int  # bank << 16 | local pc
    name: str


@dataclass
class BankCfg:
    bank: int
    includes: List[str] = field(default_factory=list)
    entries: List[BankEntry] = field(default_factory=list)
    names: List[NameDecl] = field(default_factory=list)
    exclude_ranges: List[Tuple[int, int]] = field(default_factory=list)
    data_regions: List[Tuple[int, int, int]] = field(default_factory=list)  # (bank, start, end)
    verbatim_blocks: List[str] = field(default_factory=list)


# Token regex helpers
_HEX_RE = re.compile(r'^[0-9a-fA-F]+$')


def _parse_hex(token: str) -> int:
    if token.lower().startswith('0x'):
        return int(token, 16)
    return int(token, 16)


def _strip_comment(line: str) -> str:
    """Strip trailing '# ...' comment from a cfg line."""
    idx = line.find('#')
    if idx >= 0:
        line = line[:idx]
    return line.rstrip()


def load_bank_cfg(path: str) -> BankCfg:
    """Parse a v1-format bank cfg file. Returns a BankCfg dataclass.

    Raises ValueError on a malformed `bank =` line. Other unrecognized
    directives are silently ignored (forward-compat with v1 cfgs that
    have v1-only directives the v2 pipeline doesn't need).
    """
    cfg = BankCfg(bank=-1)
    in_verbatim = False
    verbatim_buf: List[str] = []

    with open(path, 'r', encoding='utf-8', errors='replace') as fp:
        for raw in fp:
            line = raw.rstrip('\n')

            # Verbatim blocks pass through unchanged.
            if in_verbatim:
                if line.strip() == 'verbatim_end':
                    in_verbatim = False
                    cfg.verbatim_blocks.append('\n'.join(verbatim_buf))
                    verbatim_buf = []
                else:
                    verbatim_buf.append(line)
                continue
            if line.strip() == 'verbatim_start':
                in_verbatim = True
                continue

            # Strip comments + leading whitespace for everything else.
            stripped = _strip_comment(line).strip()
            if not stripped:
                continue

            tokens = stripped.split()
            head = tokens[0]

            # bank = NN
            if head == 'bank' and len(tokens) >= 3 and tokens[1] == '=':
                cfg.bank = _parse_hex(tokens[2])
                continue

            # includes = a.h b.h c.h
            if head == 'includes' and len(tokens) >= 3 and tokens[1] == '=':
                cfg.includes = list(tokens[2:])
                continue

            # comment = ... (v2 ignores)
            if head == 'comment' and '=' in stripped:
                continue

            # func <name> <hex_pc> [end:<hex_end>] [sig:...] [...]
            if head == 'func':
                if len(tokens) < 3:
                    continue
                name = tokens[1]
                start = _parse_hex(tokens[2])
                end: Optional[int] = None
                for t in tokens[3:]:
                    if t.startswith('end:'):
                        try:
                            end = _parse_hex(t[len('end:'):])
                        except ValueError:
                            pass
                cfg.entries.append(BankEntry(name=name, start=start, end=end))
                continue

            # name <hex_addr> <friendly_name> [sig:...]
            if head == 'name':
                if len(tokens) < 3:
                    continue
                try:
                    addr = _parse_hex(tokens[1])
                except ValueError:
                    continue
                friendly = tokens[2]
                cfg.names.append(NameDecl(addr_24=addr, name=friendly))
                continue

            # exclude_range <start> <end>
            if head == 'exclude_range' and len(tokens) >= 3:
                try:
                    s = _parse_hex(tokens[1])
                    e = _parse_hex(tokens[2])
                except ValueError:
                    continue
                cfg.exclude_ranges.append((s, e))
                continue

            # data_region <bank> <start> <end>
            if head == 'data_region' and len(tokens) >= 4:
                try:
                    b = _parse_hex(tokens[1])
                    s = _parse_hex(tokens[2])
                    e = _parse_hex(tokens[3])
                except ValueError:
                    continue
                cfg.data_regions.append((b, s, e))
                continue

            # Anything else: silently ignore (v1-only directive or
            # forward-compat).

    if cfg.bank < 0:
        raise ValueError(f"{path}: missing 'bank = NN' line")

    # Auto-promote in-bank `name <addr> <friendly>` declarations to emit
    # entries. v1's recompiler auto-promoted JSL/JSR targets into their
    # own functions; v2 doesn't, so JSR/JSL targets named in cfg but
    # without an explicit `func` entry would otherwise be referenced
    # without a definition. (Cross-bank `name` lines stay declaration-
    # only — their owning bank's cfg is responsible for emitting them.)
    existing_starts = {e.start & 0xFFFF for e in cfg.entries}
    for nd in cfg.names:
        if (nd.addr_24 >> 16) & 0xFF != cfg.bank:
            continue
        local_pc = nd.addr_24 & 0xFFFF
        if local_pc in existing_starts:
            continue
        cfg.entries.append(BankEntry(name=nd.name, start=local_pc))
        existing_starts.add(local_pc)

    return cfg
