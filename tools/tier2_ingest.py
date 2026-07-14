#!/usr/bin/env python3
"""
tier2_ingest.py -- audit an interpreter-tier coverage manifest.

Phase 3 of the interpreter-fallback tier (see docs/MULTI_TIER.md). Reads a
Tier-2 coverage manifest (default: build/tier2_coverage.json, schema
"snesrecomp tier2 coverage v1") that the runner writes on exit. The
manifest-driven emitter consumes clean targets directly with
`v2_emit.py --profile-manifest`; profile roots select optional AOT work while
LLE remains authoritative. This audit also proposes optional function
boundaries for unnamed targets and flags sites that need human inspection.

Human-in-the-loop BY DESIGN -- like the existing cfg_override_* proposers, it
PRINTS paste-ready directives; it does not edit cfgs. A human stays between
"observed at runtime" and "trusted as code," which is the project discipline
(no laundering a runtime mis-execution into a static translation).

Discoveries split into two buckets:

  BOUNDARY     The interpreter ran the gap and returned cleanly (clean_hits>0,
               bail_hits==0): a genuine coverage gap, safe to profile.
                 * target has no existing `func`  -> emit
                   an optional `func bank_BB_AAAA <addr16>` boundary for
                   naming/slicing. A `func` declaration is not an AOT root.
                 * target IS already a `func`     -> the gap is the dispatch
                   SITE; it needs an indirect_dispatch / indirect_call_table
                   authorization. Flagged (NOT auto-written -- the index
                   register and table layout aren't in a runtime tier-down).

  INVESTIGATE  The interpreter BAILED (bail_hits>0): it could not run the
               target -- e.g. a garbage indirect target from upstream recomp-
               state corruption (SM's JMP ($0012)=$FFFF is the canonical case).
               These are BUG LEADS, never promotion candidates. Ranked by bail
               count, then earliest frame.

Site kinds (2026-07-02 additions): besides the tier-down kinds
(indirect_dispatch / indirect_goto / bank_miss), the bridge now records
in-bridge sightings -- `call_gap` (an interpreted JSR/JSL whose target has no
compiled variant) and `goto_gap` (an indirect JMP/JML landing with none).
Both are always clean (observations, not bounded runs) and flow through the
profile. Caveat for goto_gap: a landing can be a mid-function label
(intra-function jump table) rather than a subroutine entry -- eyeball the
disassembly before pasting, as always. Addresses are LoROM-canonicalized
(exec mirrors $80-$BF recorded as $00-$3F).

Usage:
  python tools/tier2_ingest.py [manifest.json] [--cfg-dir recomp]
"""

import argparse
import json
import os
import re
import sys
from collections import defaultdict

FUNC_RE = re.compile(r'^\s*func\s+(\S+)\s+([0-9A-Fa-f]+)')
BANK_FILE_RE = re.compile(r'bank([0-9A-Fa-f]{2})\.cfg$')


def load_manifest(path):
    with open(path, 'r', encoding='utf-8') as f:
        m = json.load(f)
    schema = m.get('schema', '')
    if not schema.startswith('snesrecomp tier2 coverage'):
        sys.stderr.write(f"warning: unexpected schema {schema!r}\n")
    return m


def parse_pc24(s):
    """'0x0FE8B7' / '0FE8B7' / 1042103 -> int."""
    if isinstance(s, int):
        return s & 0xFFFFFF
    return int(str(s), 16) & 0xFFFFFF


def scan_cfg_funcs(cfg_dir):
    """Return (func_addrs, bank_files):
       func_addrs[bank] = set of in-bank 16-bit func addresses already declared;
       bank_files[bank] = path to that bank's cfg (for the paste hint)."""
    func_addrs = defaultdict(set)
    bank_files = {}
    if not os.path.isdir(cfg_dir):
        sys.stderr.write(f"warning: cfg dir {cfg_dir!r} not found -- "
                         f"can't dedup against existing funcs\n")
        return func_addrs, bank_files
    for name in sorted(os.listdir(cfg_dir)):
        mb = BANK_FILE_RE.search(name)
        if not mb:
            continue
        bank = int(mb.group(1), 16)
        bank_files[bank] = os.path.join(cfg_dir, name)
        with open(os.path.join(cfg_dir, name), 'r', encoding='utf-8',
                  errors='replace') as f:
            for line in f:
                mf = FUNC_RE.match(line)
                if mf:
                    func_addrs[bank].add(int(mf.group(2), 16) & 0xFFFF)
    return func_addrs, bank_files


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('manifest', nargs='?', default='build/tier2_coverage.json',
                    help='Tier-2 coverage manifest (default: %(default)s)')
    ap.add_argument('--cfg-dir', default='recomp',
                    help='cfg directory to dedup against (default: %(default)s)')
    ap.add_argument('--min-hits', type=int, default=1,
                    help='ignore discoveries below this total hit count')
    args = ap.parse_args()

    if not os.path.isfile(args.manifest):
        sys.stderr.write(f"error: manifest {args.manifest!r} not found. Run the "
                         f"game once (it writes the manifest on exit), then "
                         f"re-run this tool.\n")
        return 2

    m = load_manifest(args.manifest)
    func_addrs, bank_files = scan_cfg_funcs(args.cfg_dir)
    discoveries = m.get('discoveries', [])

    promote_func = defaultdict(list)   # bank -> [(addr16, disc)]
    site_needs_auth = []               # (disc) target already a func
    investigate = []                   # (disc) bailed
    seen_promote = set()               # (bank, addr16) dedup

    for d in discoveries:
        clean = int(d.get('clean_hits', 0))
        bail = int(d.get('bail_hits', 0))
        if clean + bail < args.min_hits:
            continue
        target = parse_pc24(d['target_pc24'])
        bank, addr16 = target >> 16, target & 0xFFFF
        if bail > 0:
            investigate.append(d)
            continue
        # clean-only -> promotable
        if addr16 in func_addrs.get(bank, ()):
            site_needs_auth.append(d)
        else:
            key = (bank, addr16)
            if key not in seen_promote:
                seen_promote.add(key)
                promote_func[bank].append((addr16, d))

    # -- report ------------------------------------------------------------
    out = sys.stdout.write
    out("=" * 72 + "\n")
    out(f"Tier-2 gap manifest ingest -- {m.get('rom_title', '?')}\n")
    out(f"  manifest: {args.manifest}\n")
    out(f"  total tier hits: {m.get('total_tier_hits', '?')}   "
        f"distinct (site,target,mx): {m.get('distinct_sites', len(discoveries))}"
        f"   overflowed tuples: {m.get('overflowed_tuples', 0)}\n")
    out("=" * 72 + "\n\n")

    if not discoveries:
        out("No discoveries -- the interpreter tier never fired this run.\n"
            "(For a fully-covered game that's the expected dormant state.)\n")
        return 0

    # Optional boundaries: func declarations name/slice code but do not root it.
    n_promote = sum(len(v) for v in promote_func.values())
    out("AOT optimization: pass this file to v2_emit.py with "
        "`--profile-manifest`.\n"
        "Clean target/MX observations become optional AOT roots; bails are "
        "excluded.\n\n")
    out(f"-- OPTIONAL BOUNDARIES: {n_promote} unnamed clean target(s) --\n")
    if not n_promote:
        out("  (none)\n")
    for bank in sorted(promote_func):
        cfg = bank_files.get(bank)
        hint = cfg if cfg else f"bank{bank:02x}.cfg  (NOT FOUND -- create it)"
        out(f"\n  # -> {hint}\n")
        for addr16, d in sorted(promote_func[bank]):
            kind = d.get('site_kind', '?')
            site = parse_pc24(d['site_pc24'])
            out(f"  func bank_{bank:02X}_{addr16:04X} {addr16:04x}"
                f"    # {d.get('entry_mx','?')} {kind}, "
                f"{int(d.get('clean_hits',0))} clean hit(s), "
                f"from site $%06X, first frame {d.get('first_frame','?')}\n"
                % site)

    # SITE NEEDS AUTHORIZATION: target is already a func, the dispatch site isn't.
    out(f"\n-- SITE NEEDS DISPATCH AUTHORIZATION: {len(site_needs_auth)} "
        f"site(s) --\n")
    out("  (target already has a `func`; the indirect SITE needs an\n"
        "   indirect_dispatch/indirect_call_table directive. The index reg +\n"
        "   table layout aren't in the runtime manifest, so verify against the\n"
        "   disassembly before authorizing -- not auto-generated.)\n")
    if not site_needs_auth:
        out("  (none)\n")
    for d in sorted(site_needs_auth,
                    key=lambda d: -int(d.get('clean_hits', 0))):
        site = parse_pc24(d['site_pc24'])
        target = parse_pc24(d['target_pc24'])
        out(f"  site $%06X -> target $%06X  (%s %s, %d clean hit(s))\n"
            % (site, target, d.get('entry_mx', '?'), d.get('site_kind', '?'),
               int(d.get('clean_hits', 0))))

    # INVESTIGATE: bailed sites are bug leads, not promotion candidates.
    out(f"\n-- INVESTIGATE: {len(investigate)} bailed site(s) "
        f"(bug leads, NOT promoted) --\n")
    out("  (the interpreter could not run the target -- likely an upstream\n"
        "   recomp-state bug, e.g. a garbage indirect target. Do NOT promote;\n"
        "   chase who corrupts the state that feeds this site.)\n")
    if not investigate:
        out("  (none)\n")
    for d in sorted(investigate,
                    key=lambda d: (-int(d.get('bail_hits', 0)),
                                   int(d.get('first_frame', 1 << 30)))):
        site = parse_pc24(d['site_pc24'])
        target = parse_pc24(d['target_pc24'])
        out(f"  site $%06X -> target $%06X  (%s %s, %d bail(s)/%d clean, "
            "first frame %s)\n"
            % (site, target, d.get('entry_mx', '?'), d.get('site_kind', '?'),
               int(d.get('bail_hits', 0)), int(d.get('clean_hits', 0)),
               d.get('first_frame', '?')))

    out("\n" + "=" * 72 + "\n")
    out("Regenerate with `--profile-manifest` and rebuild. Add a printed func\n"
        "only when its boundary improves naming/slicing; func is deliberately\n"
        "not a reachability root. LLE remains the fallback for every absent or\n"
        "rejected exact variant (see MULTI_TIER.md sec 3a).\n")
    return 0


if __name__ == '__main__':
    sys.exit(main())
