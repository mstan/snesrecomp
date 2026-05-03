"""Control-flow debt scanner for v2 generated bank code.

Walks every src/gen_v2/*.c and reports sites where the recompiler
emitted a comment instead of real control-flow semantics. These are
the "silent skip" cases — the compiler builds, the program runs,
but execution falls through where the original 65816 would have
branched/called/halted. This report is the input for prioritising
emitter-gap fixes; reachability is determined separately by runtime
trace.

Site types (priority order, per Codex audit + user direction):
  CALL_INDIRECT       -- /* Call indirect — caller dispatches */
  INDIRECT_GOTO       -- /* IndirectGoto: ... — caller dispatches */
  UNRESOLVABLE_GOTO   -- /* L_XXXX_MnXn unresolvable cross-fn goto ... */
  UNRESOLVED_STUB     -- caller invokes a name defined in
                         unresolved_stubs_v2.c
  BRK / COP / STP / WAI -- comment-only break/halt

Per site we capture: bank, source PC (last cpu_trace_block before
the marker), generated file:line, enclosing function, and the
comment text itself.

Usage:
  python _triage/cf_debt_report.py
      --gen-dir src/gen_v2
      [--json out.json]      # optional machine-readable sidecar
      [--filter SITE_TYPE]   # only report one type
      [--quiet-detail]       # summary only, no per-site table

Reachability is intentionally NOT determined here. We do not
classify a site as "live" or "dead". Codex's audit only proves these
sites EXIST in generated output. Verifying which ones EXECUTE in the
failing path is a separate runtime probe.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from collections import defaultdict
from dataclasses import dataclass, asdict
from typing import Dict, List, Optional, Tuple


# Recognise function-definition lines so we can attribute each marker
# to its enclosing function. Two shapes ship today: the typed-variant
# function (RecompReturn ...(_M[01]X[01])) and the alias wrapper
# (void <name>(...)). The alias wrapper bodies are short and unlikely
# to host markers, but cover them anyway.
RE_FUNC_DEF = re.compile(
    r'^(?:RecompReturn|void)\s+([A-Za-z_][A-Za-z_0-9]*)\s*\(\s*CpuState\s*\*\s*cpu\s*\)\s*\{'
)

# Block entry trace — last one before a marker tells us the source
# PC region the marker belongs to. Format: cpu_trace_block(cpu, 0xXXYYYY);
RE_BLOCK_TRACE = re.compile(r'cpu_trace_block\s*\(\s*cpu\s*,\s*0x([0-9A-Fa-f]{6})\s*\)')

# Marker patterns. Each captures a "site_type" and the human-readable
# comment text. Using contains-string checks instead of regex where
# possible — the markers are stable strings emitted by codegen.py.
# CALL_INDIRECT now prefers the tagged shape (JSR ($XXXX,X) at $BB:PPPP);
# the legacy bare comment is kept as a fallback for any pre-tag generated
# files that haven't been regen'd yet.
MARKERS: List[Tuple[str, str]] = [
    # Order matters: SUPPRESSED variant must be checked BEFORE the
    # plain CALL_INDIRECT since the substring `/* Call indirect:` is
    # contained inside both.
    ('CALL_INDIRECT_SUPPRESSED', '/* Call indirect SUPPRESSED'),
    ('CALL_INDIRECT',     '/* Call indirect: JSR ('),
    ('CALL_INDIRECT',     '/* Call indirect — caller dispatches */'),
    ('CALL_TARGET_UNK',   '/* Call: target unknown — caller dispatches */'),
    ('INDIRECT_GOTO',     '/* IndirectGoto:'),
    ('BRK',               '/* BRK: software interrupt */'),
    ('COP',               '/* COP: software interrupt */'),
    ('STP',               '/* STP: halt — runtime hook */'),
    ('WAI',               '/* WAI: wait for interrupt — runtime hook */'),
]

# Tagged CALL_INDIRECT shape:
#   /* Call indirect: JSR ($XXXX,X) at $BBPPPP — caller dispatches */
RE_CALL_INDIRECT_TAGGED = re.compile(
    r'/\*\s*Call indirect(?:\s+SUPPRESSED)?:\s*JSR\s*\(\$([0-9A-Fa-f]{4}),X\)\s+at\s+\$([0-9A-Fa-f]{6})\s*'
)

# "Unresolvable cross-fn goto" appears inside a return-comment; we
# match by substring rather than as a line-start marker because the
# enclosing line is "return RECOMP_RETURN_NORMAL; /* ... */".
UNRESOLVABLE_GOTO_SUBSTR = 'unresolvable cross-fn goto'

# v1-shape stub line in the older gen, kept defensively. Codex's
# audit found 42 instances at one point; current v2 might have 0.
UNRESOLVED_PASTEND_SUBSTR = 'unresolved (cross-fn / cross-bank / past end:)'

# Stub-definition shape inside unresolved_stubs_v2.c. Matches both the
# legacy silent body `(void)cpu; return RECOMP_RETURN_NORMAL;` and the
# loud-trap variant `return cpu_trace_unresolved_stub_trap(...);`.
RE_STUB_DEF = re.compile(
    r'^RecompReturn\s+([A-Za-z_][A-Za-z_0-9]*)\s*\(\s*CpuState\s*\*\s*cpu\s*\)\s*\{\s*'
    r'(?:\(void\)cpu;\s*return\s+RECOMP_RETURN_NORMAL'
    r'|return\s+cpu_trace_unresolved_stub_trap)'
)


@dataclass
class Site:
    site_type: str
    bank: str
    source_pc: Optional[str]   # bank-encoded 24-bit address, last seen cpu_trace_block
    file: str
    line: int
    function: Optional[str]
    comment: str
    # CALL_INDIRECT-only: parsed from the tagged comment shape. Both
    # are hex strings without `0x`/`$`. None for legacy untagged sites
    # or non-indirect-call markers.
    site_pc24: Optional[str] = None    # exact PC of the JSR (abs,X) insn
    table_base: Optional[str] = None   # operand of the JSR (abs,X)


def bank_from_filename(path: str) -> str:
    """smw_00_v2.c -> '00', smw_0c_v2.c -> '0C', etc."""
    name = os.path.basename(path)
    m = re.match(r'smw_([0-9a-fA-F]{2})_v2\.c$', name)
    if m:
        return m.group(1).upper()
    if name == 'unresolved_stubs_v2.c':
        return '--'
    return '??'


def load_stub_names(stubs_path: str) -> List[str]:
    if not os.path.exists(stubs_path):
        return []
    names: List[str] = []
    with open(stubs_path, 'r', encoding='utf-8', errors='replace') as f:
        for line in f:
            m = RE_STUB_DEF.match(line.strip())
            if m:
                names.append(m.group(1))
    return names


def scan_file_for_markers(path: str) -> List[Site]:
    """Pass 1 of 2: find every comment-only marker site.

    Tracks the enclosing function and the most-recent cpu_trace_block
    PC so each Site has full attribution.
    """
    sites: List[Site] = []
    bank = bank_from_filename(path)
    cur_func: Optional[str] = None
    last_pc: Optional[str] = None
    func_brace_depth = 0  # rough — top-level braces only

    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        for lineno, raw in enumerate(f, start=1):
            stripped = raw.strip()

            # Function entry — match before brace counting because the
            # `{` is on the same line as the def.
            m = RE_FUNC_DEF.match(stripped)
            if m:
                cur_func = m.group(1)
                func_brace_depth = 1
                last_pc = None
                continue

            # Track {} balance roughly to detect when we leave a function.
            # This isn't a real C parser — just enough to reset cur_func.
            if cur_func is not None:
                func_brace_depth += stripped.count('{') - stripped.count('}')
                if func_brace_depth <= 0:
                    cur_func = None
                    last_pc = None
                    func_brace_depth = 0

            # Block-trace gives us the source PC for any markers that
            # follow within the same block.
            mb = RE_BLOCK_TRACE.search(stripped)
            if mb:
                last_pc = mb.group(1).upper()

            # Markers — straight substring check, exact comment text.
            for site_type, needle in MARKERS:
                if needle in stripped:
                    comment = stripped
                    site = Site(
                        site_type=site_type,
                        bank=bank,
                        source_pc=last_pc,
                        file=path,
                        line=lineno,
                        function=cur_func,
                        comment=comment,
                    )
                    # If this is a tagged CALL_INDIRECT(_SUPPRESSED),
                    # parse the exact JSR site PC and table base out
                    # of the comment so callers don't have to re-parse.
                    if site_type in ('CALL_INDIRECT', 'CALL_INDIRECT_SUPPRESSED'):
                        mt = RE_CALL_INDIRECT_TAGGED.search(stripped)
                        if mt:
                            site.table_base = mt.group(1).upper()
                            site.site_pc24 = mt.group(2).upper()
                    sites.append(site)
                    break

            # Unresolvable cross-fn goto — return-line comment.
            if UNRESOLVABLE_GOTO_SUBSTR in stripped:
                sites.append(Site(
                    site_type='UNRESOLVABLE_GOTO',
                    bank=bank,
                    source_pc=last_pc,
                    file=path,
                    line=lineno,
                    function=cur_func,
                    comment=stripped,
                ))

            # Older v1-shape "unresolved" stub-return. Should be 0 in v2;
            # log if we ever see it so we know the cleanup regressed.
            if UNRESOLVED_PASTEND_SUBSTR in stripped:
                sites.append(Site(
                    site_type='UNRESOLVED_PASTEND',
                    bank=bank,
                    source_pc=last_pc,
                    file=path,
                    line=lineno,
                    function=cur_func,
                    comment=stripped,
                ))

    return sites


def scan_file_for_stub_calls(path: str, stub_names: List[str]) -> List[Site]:
    """Pass 2: locate every call site where a stub function is invoked.

    A 'call site' is a line containing `<stub_name>(cpu)`. We don't
    need the comment text here — there isn't one — but we synthesise
    a comment-equivalent for the report.
    """
    if not stub_names:
        return []
    sites: List[Site] = []
    bank = bank_from_filename(path)
    cur_func: Optional[str] = None
    last_pc: Optional[str] = None
    func_brace_depth = 0

    # Pre-build one regex matching any stub-call. Caller-side shape is
    # `<name>(cpu)` — sometimes inside an assignment or after a cast.
    # Word boundaries keep us from matching substrings.
    stub_set = set(stub_names)
    stub_re = re.compile(r'\b(' + '|'.join(re.escape(s) for s in stub_names) + r')\s*\(\s*cpu\s*\)')

    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        for lineno, raw in enumerate(f, start=1):
            stripped = raw.strip()

            m = RE_FUNC_DEF.match(stripped)
            if m:
                cur_func = m.group(1)
                func_brace_depth = 1
                last_pc = None
                continue
            if cur_func is not None:
                func_brace_depth += stripped.count('{') - stripped.count('}')
                if func_brace_depth <= 0:
                    cur_func = None
                    last_pc = None
                    func_brace_depth = 0

            mb = RE_BLOCK_TRACE.search(stripped)
            if mb:
                last_pc = mb.group(1).upper()

            sm = stub_re.search(stripped)
            if sm and sm.group(1) in stub_set:
                # Skip the stub's own definition line if we somehow
                # encounter it (we shouldn't; we exclude stubs file).
                if cur_func == sm.group(1):
                    continue
                sites.append(Site(
                    site_type='UNRESOLVED_STUB',
                    bank=bank,
                    source_pc=last_pc,
                    file=path,
                    line=lineno,
                    function=cur_func,
                    comment=f'stub call: {sm.group(1)}(cpu)',
                ))
    return sites


def format_summary(sites: List[Site]) -> str:
    by_type: Dict[str, int] = defaultdict(int)
    by_type_bank: Dict[Tuple[str, str], int] = defaultdict(int)
    for s in sites:
        by_type[s.site_type] += 1
        by_type_bank[(s.site_type, s.bank)] += 1

    lines: List[str] = []
    lines.append('CONTROL-FLOW DEBT — SUMMARY BY SITE TYPE')
    lines.append('=' * 60)
    order = [
        'CALL_INDIRECT',
        'CALL_TARGET_UNK',
        'INDIRECT_GOTO',
        'UNRESOLVABLE_GOTO',
        'UNRESOLVED_PASTEND',
        'UNRESOLVED_STUB',
        'BRK',
        'COP',
        'STP',
        'WAI',
        # Informational — not priority-1. SUPPRESSED sites are JSR
        # (abs,X) decoder phantoms severed by cfg-required-dispatch-or-
        # kill (2026-05-03). They ship loud-marker comments only; no
        # runtime behaviour.
        'CALL_INDIRECT_SUPPRESSED',
    ]
    for st in order:
        n = by_type.get(st, 0)
        if n == 0:
            continue
        per_bank = sorted(
            ((b, c) for (t, b), c in by_type_bank.items() if t == st),
            key=lambda kv: kv[0],
        )
        per_bank_str = ', '.join(f'{b}:{c}' for b, c in per_bank)
        lines.append(f'  {st:<22} {n:>4}   ({per_bank_str})')
    total = sum(by_type.values())
    lines.append('-' * 60)
    lines.append(f'  {"TOTAL":<22} {total:>4}')
    return '\n'.join(lines)


def format_detail(sites: List[Site], gen_dir: str) -> str:
    """Per-site rows, grouped by site_type, sorted by bank then PC."""
    by_type: Dict[str, List[Site]] = defaultdict(list)
    for s in sites:
        by_type[s.site_type].append(s)
    out: List[str] = []
    out.append('')
    out.append('CONTROL-FLOW DEBT — DETAIL')
    out.append('=' * 80)
    order = [
        'CALL_INDIRECT',
        'CALL_TARGET_UNK',
        'INDIRECT_GOTO',
        'UNRESOLVABLE_GOTO',
        'UNRESOLVED_PASTEND',
        'UNRESOLVED_STUB',
        'BRK',
        'COP',
        'STP',
        'WAI',
        # Informational — not priority-1. SUPPRESSED sites are JSR
        # (abs,X) decoder phantoms severed by cfg-required-dispatch-or-
        # kill (2026-05-03). They ship loud-marker comments only; no
        # runtime behaviour.
        'CALL_INDIRECT_SUPPRESSED',
    ]
    for st in order:
        rows = by_type.get(st, [])
        if not rows:
            continue
        # CALL_INDIRECT(_SUPPRESSED): sort by exact site PC when known,
        # falling back to block-trace source_pc for legacy untagged
        # entries. That groups sites within the same dispatcher loop
        # together (e.g. all $00:EF93 hits across (m,x) variants).
        if st in ('CALL_INDIRECT', 'CALL_INDIRECT_SUPPRESSED'):
            rows.sort(key=lambda s: (
                s.bank,
                s.site_pc24 or s.source_pc or '',
                s.table_base or '',
                s.file,
                s.line,
            ))
        else:
            rows.sort(key=lambda s: (s.bank, s.source_pc or '', s.file, s.line))
        out.append('')
        out.append(f'-- {st} ({len(rows)} sites) ' + '-' * (60 - len(st)))
        for s in rows:
            rel = os.path.relpath(s.file, gen_dir) if gen_dir else s.file
            fn = s.function or '<top-level>'
            if st in ('CALL_INDIRECT', 'CALL_INDIRECT_SUPPRESSED') and s.site_pc24:
                out.append(
                    f'  bank ${s.bank}  $pc:{s.site_pc24}  '
                    f'JSR (${s.table_base},X)  {rel}:{s.line}  in {fn}'
                )
            else:
                pc = f'$pc:{s.source_pc}' if s.source_pc else '$pc:??????'
                out.append(f'  bank ${s.bank}  {pc}  {rel}:{s.line}  in {fn}')
            # Truncate long comments — IndirectGoto comments include
            # the resolution expression and can be 100+ chars.
            cmt = s.comment if len(s.comment) <= 140 else s.comment[:137] + '...'
            out.append(f'      {cmt}')
    return '\n'.join(out)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen-dir', default='src/gen_v2',
                    help='Directory containing smw_XX_v2.c files')
    ap.add_argument('--stubs-file', default='src/gen_v2/unresolved_stubs_v2.c',
                    help='Generated stubs file (defines no-op bodies)')
    ap.add_argument('--json', default=None,
                    help='Optional path for JSON sidecar of all sites')
    ap.add_argument('--filter', default=None,
                    help='Only report this site_type (e.g. CALL_INDIRECT)')
    ap.add_argument('--quiet-detail', action='store_true',
                    help='Summary only — skip the per-site table')
    ap.add_argument('--with-asm', action='store_true',
                    help='[CALL_INDIRECT] decode 8 insns before + 4 after each '
                         'JSR (abs,X) site. Requires --rom and --cfg-dir.')
    ap.add_argument('--with-bounds', action='store_true',
                    help='[CALL_INDIRECT] detect CMP/CPX/AND/ASL bounds '
                         'patterns near each JSR (abs,X). Requires --rom '
                         'and --cfg-dir. Implies --with-asm.')
    ap.add_argument('--rom', default='smw.sfc',
                    help='Path to SMW ROM image (used by --with-asm/bounds)')
    ap.add_argument('--cfg-dir', default='recomp', dest='cfg_dir',
                    help='Path to bankXX.cfg directory (used by '
                         '--with-asm/bounds for function entry lookup)')
    ap.add_argument('--table-dump-count', type=int, default=8,
                    help='[CALL_INDIRECT] minimum entries to dump from each '
                         'ROM table; bounds-detected count overrides if larger')
    args = ap.parse_args()

    if not os.path.isdir(args.gen_dir):
        print(f'error: --gen-dir {args.gen_dir} does not exist', file=sys.stderr)
        return 2

    bank_files = sorted(
        os.path.join(args.gen_dir, f)
        for f in os.listdir(args.gen_dir)
        if f.endswith('.c') and f != 'unresolved_stubs_v2.c'
    )

    stub_names = load_stub_names(args.stubs_file)
    all_sites: List[Site] = []
    for bf in bank_files:
        all_sites.extend(scan_file_for_markers(bf))
    for bf in bank_files:
        all_sites.extend(scan_file_for_stub_calls(bf, stub_names))

    if args.filter:
        all_sites = [s for s in all_sites if s.site_type == args.filter]

    print(format_summary(all_sites))
    if not args.quiet_detail:
        print(format_detail(all_sites, args.gen_dir))

    # ---- asm/bounds enrichment for CALL_INDIRECT sites ----
    enriched_records = []
    if args.with_asm or args.with_bounds:
        try:
            import cf_debt_asm_context as ctx_mod
        except ImportError as e:
            print(f'\nerror: cf_debt_asm_context import failed ({e})',
                  file=sys.stderr)
            return 3
        if not os.path.exists(args.rom):
            print(f'\nerror: --rom {args.rom} not found', file=sys.stderr)
            return 3
        if not os.path.isdir(args.cfg_dir):
            print(f'\nerror: --cfg-dir {args.cfg_dir} not found', file=sys.stderr)
            return 3
        rom = open(args.rom, 'rb').read()
        if len(rom) % 1024 == 512:
            rom = rom[512:]
        cfg_funcs = ctx_mod.load_cfg_funcs(args.cfg_dir)

        # Collapse the 22 markers to unique (site_pc24, table_base) pairs
        # so we don't re-decode the same JSR for every (m,x) variant.
        unique_sites: Dict[Tuple[str, str], Site] = {}
        variants_for: Dict[Tuple[str, str], List[Site]] = defaultdict(list)
        for s in all_sites:
            if s.site_type != 'CALL_INDIRECT':
                continue
            if not s.site_pc24 or not s.table_base:
                continue
            key = (s.site_pc24, s.table_base)
            unique_sites.setdefault(key, s)
            variants_for[key].append(s)

        print('\n')
        print('CALL_INDIRECT — ASM CONTEXT + BOUNDS')
        print('=' * 78)
        print(f'{len(unique_sites)} unique (site_pc, table_base) pairs '
              f'({len(variants_for)} groups, '
              f'{sum(len(v) for v in variants_for.values())} total markers)')

        for (site_pc_str, base_str), s in sorted(unique_sites.items()):
            site_pc24 = int(site_pc_str, 16)
            table_base = int(base_str, 16)
            ctx = ctx_mod.enrich_site(
                rom=rom,
                cfg_funcs=cfg_funcs,
                site_pc24=site_pc24,
                table_base=table_base,
                function_name=s.function or '',
                table_dump_count=args.table_dump_count,
            )
            print('')
            print(ctx_mod.format_asm_context(ctx))
            print(f'  variants ({len(variants_for[(site_pc_str, base_str)])}): '
                  + ', '.join(sorted({v.function or '?' for v in
                                      variants_for[(site_pc_str, base_str)]})))
            enriched_records.append(ctx.to_dict())
        print('')

    print('')
    print(f'Stubs defined in {args.stubs_file}: {len(stub_names)}')
    if stub_names:
        print('  ' + ', '.join(stub_names))

    if args.json:
        payload = {
            'sites': [asdict(s) for s in all_sites],
        }
        if enriched_records:
            payload['call_indirect_enriched'] = enriched_records
        with open(args.json, 'w', encoding='utf-8') as f:
            json.dump(payload, f, indent=2)
        print(f'\nJSON sidecar: {args.json} ({len(all_sites)} sites'
              + (f', {len(enriched_records)} enriched'
                 if enriched_records else '')
              + ')')

    return 0


if __name__ == '__main__':
    sys.exit(main())
