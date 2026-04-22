#!/usr/bin/env python3
"""
cfg_strip_redundant — survey cfg directives that the recompiler can in
principle derive on its own, for human-reviewed incremental cleanup.

Phase 3 of the cfg-parser-overhaul. Two analysis passes:

  1. `end:HHHH` directives whose HHHH is matched (or undershot) by
     discover.py's per-function end-walker.

  2. Pure-AUTO `func` lines that the discoverer would recreate
     identically (no end:, no sig override, no other hints).

WARNING: bulk --apply is NOT safe. Empirically, stripping these
unconditionally regresses the gen-C: the recompiler's funcs_with_end
fallback chain (next-non-skipped-start) does NOT match the
discoverer's per-function end for cfg-declared entries today. Phase 2
plumbed discoverer-end into auto-promoted entries only; extending it
to all cfg entries breaks downstream sub-entry promotion and dispatch
classification (validated 2026-04-22).

Use this tool to SURVEY scope and to selectively strip individual
directives the human author has reviewed and confirmed redundant. The
broader plumbing fix (extend Phase 2 to cfg-declared entries safely)
is a separate, deeper recompiler change.

Usage:
    python snesrecomp/tools/cfg_strip_redundant.py --all          # dry-run survey
    python snesrecomp/tools/cfg_strip_redundant.py --pass1 --apply # bulk strip end: (RISKY)
"""
import argparse
import pathlib
import re
import sys
from typing import Dict, List, Set, Tuple

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / 'recompiler'))

from snes65816 import load_rom  # noqa: E402
import discover  # noqa: E402
import recomp  # noqa: E402

PARENT = pathlib.Path('F:/Projects/SuperMarioWorldRecomp')
CFG_DIR = PARENT / 'recomp'
ROM_PATH = PARENT / 'smw.sfc'
BANKS = (0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x07, 0x0c, 0x0d)

# Match a cfg `func` line. Tolerates column-aligned or single-spaced.
FUNC_RE = re.compile(
    r'^(?P<lead>\s*)func\s+(?P<name>\S+)\s+(?P<addr>[0-9a-fA-F]{4})(?P<rest>.*)$'
)
# Hints we treat as "real human content" (do NOT strip the line in pass 2).
PRESERVED_HINT_PREFIXES = (
    'sig:', 'rep:', 'repx:', 'sep:', 'init_y:', 'init_carry:',
    'restores_x:', 'y_after:', 'x_after:',
)
PRESERVED_HINT_TOKENS = ('carry_ret', 'ret_y', 'no_autodiscover')


def _read_cfg_lines(path: pathlib.Path) -> Tuple[List[str], bytes]:
    raw = path.read_bytes()
    nl = b'\r\n' if b'\r\n' in raw else b'\n'
    text = raw.decode('utf-8', errors='replace')
    lines = text.split(nl.decode('utf-8'))
    return lines, nl


def _write_cfg_lines(path: pathlib.Path, lines: List[str], nl: bytes) -> None:
    body = nl.decode('utf-8').join(lines)
    path.write_bytes(body.encode('utf-8'))


def _discoverer_ends(rom: bytes, bank: int) -> Dict[int, int]:
    """Return {addr -> exclusive end} from a single discover_bank call
    seeded with the cfg's own funcs (matches recomp.py's first round)."""
    cp = CFG_DIR / f'bank{bank:02x}.cfg'
    if not cp.exists(): return {}
    cfg = recomp.parse_config(str(cp))
    seeds = {a for _, a, *_ in cfg.funcs}
    _disc, _jsl, ends = discover.discover_bank(
        rom, bank, external_seeds=seeds, return_ends=True)
    return ends


def pass1_strip_redundant_end(rom: bytes, bank: int, apply: bool) -> Tuple[int, int]:
    """Strip end:HHHH where the recompiler will compute the same (or wider).
    Returns (stripped, kept)."""
    cp = CFG_DIR / f'bank{bank:02x}.cfg'
    if not cp.exists(): return (0, 0)
    ends = _discoverer_ends(rom, bank)
    lines, nl = _read_cfg_lines(cp)
    stripped = 0
    kept = 0
    out = []
    for line in lines:
        m = FUNC_RE.match(line)
        if not m:
            out.append(line); continue
        try:
            addr = int(m.group('addr'), 16) & 0xFFFF
        except ValueError:
            out.append(line); continue
        rest = m.group('rest')
        # Find an end: token in rest.
        em = re.search(r'(\s+)end:([0-9a-fA-F]+)\b', rest)
        if not em:
            out.append(line); continue
        cfg_end = int(em.group(2), 16)
        derived_end = ends.get(addr)
        if derived_end is None or derived_end < cfg_end:
            # Discoverer can't reproduce or undershoots — keep the cfg's
            # human-supplied bound.
            kept += 1
            out.append(line); continue
        # Discoverer's end is >= cfg's end → safe to strip.
        new_rest = rest[:em.start()] + rest[em.end():]
        new_line = f"{m.group('lead')}func {m.group('name')} {m.group('addr')}{new_rest}"
        out.append(new_line)
        stripped += 1
    if apply and stripped:
        _write_cfg_lines(cp, out, nl)
    return (stripped, kept)


def pass2_strip_pure_auto(rom: bytes, bank: int, apply: bool) -> Tuple[int, int]:
    """Strip whole `func` lines that the discoverer + auto-promote would
    recreate identically (no `end:`, no sig override beyond void(), no
    other hints). Comment lines are preserved.

    Returns (stripped, kept)."""
    cp = CFG_DIR / f'bank{bank:02x}.cfg'
    if not cp.exists(): return (0, 0)
    cfg = recomp.parse_config(str(cp))
    seeds = {a for _, a, *_ in cfg.funcs}
    discovered, _jsl = discover.discover_bank(rom, bank, external_seeds=seeds)
    lines, nl = _read_cfg_lines(cp)
    stripped = 0
    kept = 0
    out = []
    for line in lines:
        m = FUNC_RE.match(line)
        if not m:
            out.append(line); continue
        try:
            addr = int(m.group('addr'), 16) & 0xFFFF
        except ValueError:
            out.append(line); continue
        # Tokenize the rest (sans inline comment) to inspect hints.
        rest = m.group('rest')
        comment_idx = rest.find('#')
        body = rest[:comment_idx] if comment_idx >= 0 else rest
        tokens = body.split()
        has_real_hint = False
        for t in tokens:
            if t.startswith('end:'): continue
            if t == 'sig:void()' or t == 'sig:void': continue
            if any(t.startswith(p) for p in PRESERVED_HINT_PREFIXES):
                has_real_hint = True; break
            if t in PRESERVED_HINT_TOKENS:
                has_real_hint = True; break
        if has_real_hint:
            kept += 1
            out.append(line); continue
        if addr not in discovered:
            # Discoverer wouldn't recreate this — keep.
            kept += 1
            out.append(line); continue
        # Strippable. Drop the entire line. Leave any preceding comment
        # block in place — it documents whatever the human noted.
        stripped += 1
    if apply and stripped:
        _write_cfg_lines(cp, [
            l for l in lines
            if not (FUNC_RE.match(l) and _line_is_strippable(l, discovered))
        ], nl)
    return (stripped, kept)


def _line_is_strippable(line: str, discovered: Set[int]) -> bool:
    m = FUNC_RE.match(line)
    if not m: return False
    try:
        addr = int(m.group('addr'), 16) & 0xFFFF
    except ValueError:
        return False
    if addr not in discovered: return False
    rest = m.group('rest')
    comment_idx = rest.find('#')
    body = rest[:comment_idx] if comment_idx >= 0 else rest
    for t in body.split():
        if t.startswith('end:'): continue
        if t in ('sig:void()', 'sig:void'): continue
        if any(t.startswith(p) for p in PRESERVED_HINT_PREFIXES): return False
        if t in PRESERVED_HINT_TOKENS: return False
    return True


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument('--pass1', action='store_true',
                   help='Strip redundant end: directives only')
    p.add_argument('--pass2', action='store_true',
                   help='Strip pure-AUTO func lines only')
    p.add_argument('--all', action='store_true', help='Both passes')
    p.add_argument('--apply', action='store_true',
                   help='Actually mutate cfgs (default: dry-run)')
    args = p.parse_args()

    if not (args.pass1 or args.pass2 or args.all):
        p.error('Specify --pass1, --pass2, or --all')

    do_pass1 = args.pass1 or args.all
    do_pass2 = args.pass2 or args.all

    rom = load_rom(str(ROM_PATH))
    grand_p1 = (0, 0)
    grand_p2 = (0, 0)

    if do_pass1:
        print('=== Pass 1: strip redundant end: directives ===')
        for bank in BANKS:
            s, k = pass1_strip_redundant_end(rom, bank, args.apply)
            print(f'  bank {bank:02x}: stripped {s}, kept {k}')
            grand_p1 = (grand_p1[0] + s, grand_p1[1] + k)
        print(f'  TOTAL: stripped {grand_p1[0]}, kept {grand_p1[1]}')
        print()

    if do_pass2:
        print('=== Pass 2: strip pure-AUTO func lines ===')
        for bank in BANKS:
            s, k = pass2_strip_pure_auto(rom, bank, args.apply)
            print(f'  bank {bank:02x}: stripped {s}, kept {k}')
            grand_p2 = (grand_p2[0] + s, grand_p2[1] + k)
        print(f'  TOTAL: stripped {grand_p2[0]}, kept {grand_p2[1]}')
        print()

    if not args.apply:
        print('DRY-RUN. Use --apply to mutate cfgs.')
        print('After applying: regen all banks and verify gen-C diff is empty.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
