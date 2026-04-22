#!/usr/bin/env python3
"""
cfg_audit — quantify how much of recomp/bank??.cfg is fossilized
discoverer output (safe to strip in the cfg-parser overhaul) vs.
genuine human knowledge that must remain.

For each `func` line in each bank's cfg, classifies it as:

  PURE_AUTO          — discoverer found this addr AND the cfg line has
                       no override hints (no end:, no sig:, no init_y,
                       no carry_ret, etc). Safe to remove from cfg
                       once recomp.py imports discover.py at parse-config
                       time.

  AUTO_WITH_END      — discoverer found this addr, cfg line has explicit
                       end:. Keep as bound override (until we teach
                       discover.py to compute end addresses too — then
                       most can be re-classified PURE_AUTO).

  AUTO_WITH_SIG      — discoverer found this addr, cfg has sig: override.
                       Keep as ABI hint until discover.py infers sigs.

  AUTO_NEEDS_END     — discoverer found this addr, NO end: directive
                       in cfg. This is the koopa-spawn bug shape: the
                       implicit end (next discovered label) may be
                       wrong. Each one is a candidate misbound function.

  MANUAL             — discoverer did NOT find this addr. cfg author
                       knows about it via some out-of-band means
                       (smwdisx symbol, savestate trace, hand reverse).
                       Keep.

  MANUAL_WITH_FULL   — manual + has end + sig. Most legitimate cfg
                       lines look like this.

Usage:
    python snesrecomp/tools/cfg_audit.py
    python snesrecomp/tools/cfg_audit.py --bank 02
    python snesrecomp/tools/cfg_audit.py --report-only PURE_AUTO
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


# REPO is .../snesrecomp; the parent SMW recomp dir is reached via the
# junction. Use the absolute parent path explicitly.
PARENT = pathlib.Path('F:/Projects/SuperMarioWorldRecomp')
CFG_DIR = PARENT / 'recomp'
ROM_PATH = PARENT / 'smw.sfc'

FUNC_RE = re.compile(r'^func\s+(\S+)\s+([0-9a-fA-F]+)(.*?)(\s*#\s*(AUTO|MANUAL).*)?$')
HINT_TOKENS = ('end:', 'sig:', 'rep:', 'repx:', 'sep:', 'init_y:',
               'carry_ret', 'no_autodiscover')


def classify(addr: int, hints_str: str, in_discovered: bool) -> str:
    """Return one of PURE_AUTO / AUTO_WITH_END / AUTO_WITH_SIG /
    AUTO_NEEDS_END / MANUAL / MANUAL_WITH_FULL."""
    has_end = 'end:' in hints_str
    has_sig = 'sig:' in hints_str
    has_other = any(h in hints_str for h in HINT_TOKENS if h not in ('end:', 'sig:'))

    if in_discovered:
        if not has_end and not has_sig and not has_other:
            return 'PURE_AUTO'
        if has_end:
            return 'AUTO_WITH_END'
        if has_sig and not has_end:
            # sig override but no end; the end-bound issue still applies.
            return 'AUTO_WITH_SIG'
        return 'PURE_AUTO'  # only "other" hints, classify defensively

    # Not in discovered.
    if has_end and has_sig:
        return 'MANUAL_WITH_FULL'
    return 'MANUAL'


def audit_bank(bank: int, rom: bytes) -> Dict[str, List[Tuple[str, int, str]]]:
    """Return classification → list of (name, addr, hints_str)."""
    cfg_path = CFG_DIR / f'bank{bank:02x}.cfg'
    if not cfg_path.exists():
        return {}

    # Run discover fresh.
    discovered, _jsl = discover.discover_bank(rom, bank)
    discovered_full = {(bank << 16) | a for a in discovered}

    buckets: Dict[str, List[Tuple[str, int, str]]] = {
        'PURE_AUTO': [], 'AUTO_WITH_END': [], 'AUTO_WITH_SIG': [],
        'AUTO_NEEDS_END': [], 'MANUAL': [], 'MANUAL_WITH_FULL': [],
    }

    with open(cfg_path) as f:
        for raw in f:
            line = raw.rstrip()
            m = FUNC_RE.match(line)
            if not m:
                continue
            name, addr_hex, hints, _trailing, _marker = m.groups()
            addr = int(addr_hex, 16)
            full_addr = (bank << 16) | addr
            in_discovered = full_addr in discovered_full
            cls = classify(addr, hints or '', in_discovered)

            # Refinement: a PURE_AUTO with no end: + presence of "RECOMP_WARN
            # treated as return" downstream is the koopa-bug shape.
            # We don't load gen-C here; classify defensively as
            # AUTO_NEEDS_END for in_discovered + no end:.
            if cls == 'PURE_AUTO' and 'end:' not in (hints or ''):
                cls = 'AUTO_NEEDS_END'

            buckets[cls].append((name, addr, (hints or '').strip()))
    return buckets


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument('--bank', type=lambda x: int(x, 16), default=None)
    p.add_argument('--report-only', choices=['PURE_AUTO', 'AUTO_WITH_END',
                   'AUTO_WITH_SIG', 'AUTO_NEEDS_END', 'MANUAL', 'MANUAL_WITH_FULL'])
    p.add_argument('--list', action='store_true',
                   help='List individual entries (default: counts only)')
    args = p.parse_args()

    if not ROM_PATH.exists():
        print(f'ROM not found at {ROM_PATH}', file=sys.stderr); return 1
    rom = load_rom(str(ROM_PATH))

    banks = [args.bank] if args.bank is not None else \
            [0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x07, 0x0c, 0x0d]

    grand: Dict[str, int] = {k: 0 for k in
                             ['PURE_AUTO', 'AUTO_WITH_END', 'AUTO_WITH_SIG',
                              'AUTO_NEEDS_END', 'MANUAL', 'MANUAL_WITH_FULL']}

    for bank in banks:
        print(f'=== bank {bank:02x} ===')
        buckets = audit_bank(bank, rom)
        if not buckets:
            print('  (no cfg)'); continue
        for cls in ('PURE_AUTO', 'AUTO_WITH_END', 'AUTO_WITH_SIG',
                    'AUTO_NEEDS_END', 'MANUAL', 'MANUAL_WITH_FULL'):
            cnt = len(buckets[cls])
            grand[cls] += cnt
            tag = '*' if cls == 'AUTO_NEEDS_END' and cnt > 0 else ' '
            print(f'  {tag} {cls:<22} : {cnt}')
            if args.list and (not args.report_only or args.report_only == cls):
                for name, addr, hints in buckets[cls]:
                    h = (' ' + hints) if hints else ''
                    print(f'      {addr:04x}  {name:<60}{h}')
        print()

    print('=== GRAND TOTAL ===')
    total = sum(grand.values())
    for cls in ('PURE_AUTO', 'AUTO_WITH_END', 'AUTO_WITH_SIG',
                'AUTO_NEEDS_END', 'MANUAL', 'MANUAL_WITH_FULL'):
        pct = 100.0 * grand[cls] / total if total else 0
        print(f'  {cls:<22} : {grand[cls]:>5} ({pct:5.1f}%)')
    print(f'  {"total":<22} : {total:>5}')
    print()
    # Cross-reference gen-C RECOMP_WARN entries against cfg funcs.
    # Each warn that says "treated as return" means a branch landed
    # outside an auto-bounded function — almost always the koopa-bug
    # shape. Map each warn back to the enclosing cfg func entry so we
    # have a precise actively-buggy list.
    print('=== ACTIVE BUGS (RECOMP_WARN cross-reference) ===')
    gen_dir = PARENT / 'src' / 'gen'
    warn_re = re.compile(
        r'/\* RECOMP_WARN:\s+(\S+)\s+\$([0-9a-fA-F]+)\s+treated as return.*?'
        r"Fix:.*?'end:([0-9a-fA-F]+)'",
        re.DOTALL,
    )
    func_def_re = re.compile(r'^\s*\S+\s+(\w+)\s*\([^)]*\)\s*\{\s*//\s*([0-9a-fA-F]+)')

    # For each bank, build a sorted list of all *defined* symbol addresses
    # (func + name). This gives us a safe upper bound for end:: the next
    # defined symbol after the branch target. Using branch_target as end:
    # is off-by-one (end: is exclusive), so the recompiler's suggestion of
    # `end:{branch_target}` is always insufficient. Use next-symbol instead.
    cfg_addr_re = re.compile(r'^(?:func|name)\s+\S+\s+([0-9a-fA-F]{4,6})', re.MULTILINE)
    bank_symbols: Dict[int, List[int]] = {}
    for bank_hex in (0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x07, 0x0c, 0x0d):
        cp = CFG_DIR / f'bank{bank_hex:02x}.cfg'
        if not cp.exists(): continue
        addrs = set()
        for m in cfg_addr_re.finditer(cp.read_text(encoding='utf-8', errors='ignore')):
            v = int(m.group(1), 16) & 0xFFFF
            addrs.add(v)
        bank_symbols[bank_hex] = sorted(addrs)

    def next_symbol_after(bank_hex: int, addr: int) -> int:
        """Return smallest symbol addr in this bank > addr, or 0x10000 if none."""
        for v in bank_symbols.get(bank_hex, ()):
            if v > addr:
                return v
        return 0x10000

    actives: List[Tuple[int, int, str, str, int, int]] = []  # (bank, fn_addr, fn_name, br_kind, br_target, fix_end)
    for bank_hex in (0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x07, 0x0c, 0x0d):
        gp = gen_dir / f'smw_{bank_hex:02x}_gen.c'
        if not gp.exists(): continue
        text = gp.read_text(encoding='utf-8', errors='ignore')
        # Build a sorted list of (line, fn_addr, fn_name) for enclosing-fn lookup.
        defs = []
        for i, line in enumerate(text.splitlines()):
            m = func_def_re.match(line)
            if m:
                defs.append((i, int(m.group(2), 16), m.group(1)))
        for warn in warn_re.finditer(text):
            br_kind = warn.group(1)
            br_target = int(warn.group(2), 16)
            # Recompiler suggests end:{branch_target} but end: is exclusive,
            # so the target stays out-of-range. Replace with next-symbol bound.
            fix_end = next_symbol_after(bank_hex, br_target)
            warn_line = text.count('\n', 0, warn.start())
            # Find latest def whose line < warn_line.
            enclosing = None
            for line_no, addr, name in defs:
                if line_no <= warn_line:
                    enclosing = (addr, name)
                else:
                    break
            if enclosing:
                actives.append((bank_hex, enclosing[0], enclosing[1], br_kind, br_target, fix_end))
    if actives:
        print(f'  {len(actives)} actively-buggy cfg entries:')
        print(f'  {"bank":<5} {"func_addr":<11} {"name":<48} {"branch":<8} -> {"target":<7}  fix')
        for bank, faddr, fname, brk, btgt, fend in actives:
            bs = f'{bank:02x}'
            print(f'  {bs:<5} {faddr:>06x}      {fname:<48} {brk:<8} -> ${btgt:04x}    end:{fend:04x}')
    else:
        print('  (none — all RECOMP_WARN return-defects already fixed)')
    print()
    print('Interpretation:')
    print('  PURE_AUTO       — fossilized discoverer output, safe to strip')
    print('                    after phase 2 lands.')
    print('  AUTO_NEEDS_END  — bug-shape: discovered addr without end:')
    print('                    override. Each is a candidate koopa-style')
    print('                    misbound function.')
    print('  AUTO_WITH_END   — partial overrides; end: is real cfg work.')
    print('  AUTO_WITH_SIG   — ABI hint, keep until discoverer infers sigs.')
    print('  MANUAL*         — genuine human knowledge, always keep.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
