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
unconditionally regresses the gen-C. Diagnosed on 2026-04-22: the
cfg `end:X` directive bounds multiple recompiler passes, not just the
single func's decode:

  1. decode_func's `pc >= end` termination for THIS function.
  2. promote_sub_entries' enclosing-range check: a `name` line inside
     [F_start, end) becomes a sub-entry of F. Without end:, the
     effective range widens (to next-non-skip), so MORE `name` lines
     get split out as sub-entries, changing parent/child decomposition
     iteratively.
  3. auto_promote_branch_targets' range check: a branch target
     inside [F_start, end) gets an auto_BB_AAAA name generated.
     Widening the range changes which targets get named.
  4. _auto_detect_dispatch_helpers' body decode (Phase 2 plumbed
     _next_addr_for here, but it's still pre-sub-entry-promotion).

Passes 2-3 iterate, so "identity" of cfg_end == next-non-skip at
parse-time doesn't survive the full pipeline. Attempted "safe" strip
(cfg_end == next-non-skip including simulated auto-promote additions)
still regressed 11 RECOMP_WARN defects because sub-entry promotion
saw a different decomposition after the strip.

Use this tool to SURVEY scope. Selective stripping of individual
directives is possible but requires per-line human review plus a
full regen-and-audit cycle. The broader plumbing fix — decoupling
sub-entry promotion from the cfg func's range, so end: only bounds
the single func's own decode and not downstream passes — is a
substantial recompiler change (deferred).

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
    """Strip end:HHHH only when HHHH == next-non-skipped-func-start (i.e.,
    stripping the directive yields identical decode behavior — the default
    fallback produces the same end).

    Original, unsafe attempt: strip whenever derived_end >= cfg_end. That
    regressed 7 RECOMP_WARN defects because for cfg-declared funcs whose
    body extends past a sibling function (e.g. ParseLevelSpriteList_Entry2
    crossing FindFreeNormalSpriteSlot_LowPriority to reach $ab78), the
    discoverer's per-function walk stops at the sibling and reports a
    narrower d_end than the real body. The manual end: exists precisely
    for that case — stripping it deletes load-bearing information.

    Identity strip is always safe: behavior does not change at all.

    Returns (stripped, kept)."""
    cp = CFG_DIR / f'bank{bank:02x}.cfg'
    if not cp.exists(): return (0, 0)
    cfg = recomp.parse_config(str(cp))
    # Simulate the auto-promote pass so next-non-skip reflects the
    # POST-auto-promote ordering that the emitter actually sees.
    # Without this, cfg-declared entries can have cfg_end pointing at a
    # sibling that the cfg author named, but the auto-promote pass
    # subsequently inserts new auto_BB_AAAA entries between them. The
    # emitter's next-non-skip falls on the auto entry (narrower than
    # cfg_end), so stripping cfg_end regresses those decode ranges.
    seeds = {a for _, a, *_ in cfg.funcs}
    discovered, _ = discover.discover_bank(
        rom, bank, external_seeds=seeds,
        jsl_dispatch=set(cfg.jsl_dispatch or []),
        jsl_dispatch_long=set(cfg.jsl_dispatch_long or []))
    existing_addrs = {a for _, a, *_ in cfg.funcs}
    existing_names = {a & 0xFFFF for a in cfg.names if (a >> 16) == bank}
    simulated_auto_addrs = set()
    for a in discovered:
        if a in existing_addrs: continue
        if a in existing_names: continue
        if a in cfg.no_autodiscover: continue
        if any(er_s <= a <= er_e for er_s, er_e in cfg.exclude_ranges):
            continue
        simulated_auto_addrs.add(a)
    non_skip = sorted(
        [a for n, a, *_ in cfg.funcs if n not in cfg.skip]
        + list(simulated_auto_addrs)
    )
    def _next_non_skip(addr: int) -> int:
        for a in non_skip:
            if a > addr: return a
        return 0x10000
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
        em = re.search(r'(\s+)end:([0-9a-fA-F]+)\b', rest)
        if not em:
            out.append(line); continue
        cfg_end = int(em.group(2), 16)
        nns = _next_non_skip(addr)
        if cfg_end != nns:
            # Stripping would change behavior — cfg_end is load-bearing.
            kept += 1
            out.append(line); continue
        # cfg_end == next-non-skip: directive is documentation of the
        # fallback. Strip safely.
        new_rest = rest[:em.start()] + rest[em.end():]
        new_line = f"{m.group('lead')}func {m.group('name')} {m.group('addr')}{new_rest}"
        out.append(new_line)
        stripped += 1
    if apply and stripped:
        _write_cfg_lines(cp, out, nl)
    return (stripped, kept)


def pass2_strip_pure_auto(rom: bytes, bank: int, apply: bool) -> Tuple[int, int]:
    """Strip whole `func` lines that the discoverer + auto-promote would
    recreate identically. A line is strippable only if ALL of:

     - discoverer would find the addr (so auto-promote fires for it).
     - line has no override hints (no end:, sig, init_y, etc.).
     - NO standalone `name` entry exists at the same addr — those block
       auto-promote from re-adding the func (recomp.py:5873).
     - The name on the func line matches the `auto_BB_AAAA` shape that
       auto-promote would generate. If the cfg author gave it a
       semantic name (e.g. `HandleSPCUploads_UploadSPCEngine`), that
       name carries human knowledge and re-adding as `auto_00_80E8`
       would regress every caller's symbol reference.

    Returns (stripped, kept)."""
    cp = CFG_DIR / f'bank{bank:02x}.cfg'
    if not cp.exists(): return (0, 0)
    cfg = recomp.parse_config(str(cp))
    # Check whether auto-promote would RE-FIND each addr if the cfg
    # entry were removed. Running discover.py with the FULL seed set
    # includes the addr itself (trivially "discovered" just because we
    # seeded it), so that test is tautological. Instead, seed discover
    # with only the func entries NOT at this addr, then check.
    all_seeds = {a for _, a, *_ in cfg.funcs}
    discovered, _jsl = discover.discover_bank(
        rom, bank, external_seeds=all_seeds)
    # For strip-safety, run a second discovery with NO external seeds
    # (vectors-only + brute-force JSL scan). Addresses in this set are
    # what auto-promote would find on a fresh regen starting from an
    # empty cfg + the discovered addresses. Only these are safely
    # strippable.
    organically_discovered, _jsl2 = discover.discover_bank(
        rom, bank, external_seeds=set())
    # Addresses that have a STANDALONE `name` line (NOT the auto-added
    # name entry that `func` lines also populate — those are not
    # separate cfg lines). Standalone `name` lines block auto-promote's
    # re-addition via the cfg.names check (recomp.py:5873).
    standalone_name_addrs: Set[int] = set()
    _name_re = re.compile(r'^\s*name\s+([0-9a-fA-F]{4,6})\b')
    raw_text = cp.read_text(encoding='utf-8', errors='replace')
    for rline in raw_text.splitlines():
        m = _name_re.match(rline)
        if m:
            standalone_name_addrs.add(int(m.group(1), 16) & 0xFFFF)
    name_addrs = standalone_name_addrs
    # Auto-promote regenerates entries as `auto_BB_AAAA`. Only lines
    # whose fname matches that EXACT shape can be stripped without
    # losing the cfg author's semantic name.
    auto_name_re = re.compile(
        rf'^auto_{bank:02X}_[0-9A-F]{{4}}$'
    )
    lines, nl = _read_cfg_lines(cp)
    strippable: Set[int] = set()
    kept_count = 0
    for line in lines:
        m = FUNC_RE.match(line)
        if not m: continue
        try:
            addr = int(m.group('addr'), 16) & 0xFFFF
        except ValueError:
            continue
        fname = m.group('name')
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
            kept_count += 1; continue
        if addr not in organically_discovered:
            # This func would NOT be re-discovered by auto-promote on
            # a fresh regen. Stripping it would lose the symbol.
            kept_count += 1; continue
        if addr in name_addrs:
            kept_count += 1; continue
        if not auto_name_re.match(fname):
            # Human-named entry — carries semantic information. Strip
            # would regress every caller's symbol.
            kept_count += 1; continue
        strippable.add(addr)
    if apply and strippable:
        out = [l for l in lines
               if not (FUNC_RE.match(l) and _line_is_strippable(l, organically_discovered, name_addrs, auto_name_re))]
        _write_cfg_lines(cp, out, nl)
    return (len(strippable), kept_count)


def _line_is_strippable(line: str, discovered: Set[int],
                         name_addrs: Set[int],
                         auto_name_re) -> bool:
    m = FUNC_RE.match(line)
    if not m: return False
    try:
        addr = int(m.group('addr'), 16) & 0xFFFF
    except ValueError:
        return False
    if addr not in discovered: return False
    if addr in name_addrs: return False
    if not auto_name_re.match(m.group('name')): return False
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
