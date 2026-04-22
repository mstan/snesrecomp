#!/usr/bin/env python3
"""
cfg_marker_audit — flag cfg `# AUTO` / `# MANUAL` markers whose content
doesn't match the marker.

Phase 4 scope: rather than mass-renaming markers (high risk for cosmetic
gain), classify what each existing marker really represents and surface
mismatches the human author should review:

  AUTO_WITH_HINTS — line is marked `# AUTO` but has end:/sig:/init_y/etc.
                    overrides. The marker is wrong: a human added content
                    on top of the auto-discovery line. Either re-mark as
                    MANUAL or document the hint.

  MANUAL_WITHOUT_CONTENT — line is marked `# MANUAL` but has no explicit
                            end:, no non-default sig, no other hints.
                            Either the marker is wrong (the entry is
                            actually pure-AUTO content) or the human
                            named it via the `name` line in addition.
                            Either way, the `# MANUAL` marker carries
                            no real signal — survey only.

The script does NOT modify any cfg. Markers are documentation.
Renaming them won't change recompiler behavior — the recompiler
ignores the # comment entirely. So this is a documentation-correctness
survey, useful for hand-curating the cfgs over time but not for
automated cleanup.

Usage:
    python snesrecomp/tools/cfg_marker_audit.py
    python snesrecomp/tools/cfg_marker_audit.py --bank 02 --list
"""
import argparse
import pathlib
import re
import sys
from typing import Dict, List, Tuple

PARENT = pathlib.Path('F:/Projects/SuperMarioWorldRecomp')
CFG_DIR = PARENT / 'recomp'
BANKS = (0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x07, 0x0c, 0x0d)

FUNC_RE = re.compile(
    r'^\s*func\s+(?P<name>\S+)\s+(?P<addr>[0-9a-fA-F]{4})(?P<rest>.*)$'
)
HINT_PREFIXES = ('end:', 'sig:', 'rep:', 'repx:', 'sep:', 'init_y:',
                 'init_carry:', 'restores_x:', 'y_after:', 'x_after:')
HINT_TOKENS = ('carry_ret', 'ret_y', 'no_autodiscover')


def _line_marker(rest: str) -> str:
    """Return 'AUTO', 'MANUAL', or 'OTHER'/'NONE' based on the inline #."""
    idx = rest.find('#')
    if idx < 0: return 'NONE'
    comment = rest[idx+1:].strip()
    head = comment.split()[0] if comment else ''
    head = head.rstrip(':;,')
    if head == 'AUTO': return 'AUTO'
    if head == 'MANUAL': return 'MANUAL'
    return 'OTHER'


def _line_has_hint(rest: str) -> bool:
    """True if rest contains any non-default override hint."""
    idx = rest.find('#')
    body = rest[:idx] if idx >= 0 else rest
    for t in body.split():
        if t in ('sig:void()', 'sig:void'): continue
        if any(t.startswith(p) for p in HINT_PREFIXES): return True
        if t in HINT_TOKENS: return True
    return False


def audit_bank(bank: int) -> Dict[str, List[Tuple[int, str, str]]]:
    cp = CFG_DIR / f'bank{bank:02x}.cfg'
    if not cp.exists(): return {}
    buckets: Dict[str, List[Tuple[int, str, str]]] = {
        'AUTO_WITH_HINTS': [],
        'MANUAL_WITHOUT_CONTENT': [],
        'AUTO_CLEAN': [],
        'MANUAL_WITH_CONTENT': [],
        'OTHER_MARKER': [],
        'NO_MARKER': [],
    }
    for raw in cp.read_text(encoding='utf-8', errors='replace').splitlines():
        m = FUNC_RE.match(raw)
        if not m: continue
        try:
            addr = int(m.group('addr'), 16) & 0xFFFF
        except ValueError:
            continue
        name = m.group('name')
        rest = m.group('rest')
        marker = _line_marker(rest)
        has_hint = _line_has_hint(rest)
        if marker == 'AUTO':
            buckets['AUTO_WITH_HINTS' if has_hint else 'AUTO_CLEAN'].append(
                (addr, name, rest.strip()))
        elif marker == 'MANUAL':
            buckets['MANUAL_WITH_CONTENT' if has_hint else 'MANUAL_WITHOUT_CONTENT'].append(
                (addr, name, rest.strip()))
        elif marker == 'OTHER':
            buckets['OTHER_MARKER'].append((addr, name, rest.strip()))
        else:
            buckets['NO_MARKER'].append((addr, name, rest.strip()))
    return buckets


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument('--bank', type=lambda x: int(x, 16))
    p.add_argument('--list', action='store_true')
    args = p.parse_args()

    banks = [args.bank] if args.bank is not None else list(BANKS)
    grand: Dict[str, int] = {
        'AUTO_WITH_HINTS': 0, 'MANUAL_WITHOUT_CONTENT': 0,
        'AUTO_CLEAN': 0, 'MANUAL_WITH_CONTENT': 0,
        'OTHER_MARKER': 0, 'NO_MARKER': 0,
    }
    for bank in banks:
        print(f'=== bank {bank:02x} ===')
        buckets = audit_bank(bank)
        if not buckets:
            print('  (no cfg)'); continue
        for cls in grand:
            cnt = len(buckets[cls])
            grand[cls] += cnt
            tag = '*' if cls in ('AUTO_WITH_HINTS', 'MANUAL_WITHOUT_CONTENT') and cnt > 0 else ' '
            print(f'  {tag} {cls:<24} : {cnt}')
            if args.list and cnt > 0:
                for addr, name, rest in buckets[cls]:
                    print(f'      {addr:04x}  {name:<48} {rest}')
        print()

    print('=== GRAND TOTAL ===')
    total = sum(grand.values())
    for cls, cnt in grand.items():
        pct = 100.0 * cnt / total if total else 0
        print(f'  {cls:<24} : {cnt:>5} ({pct:5.1f}%)')
    print(f'  {"total":<24} : {total:>5}')
    print()
    print('Mismatch classes (worth human review):')
    print('  AUTO_WITH_HINTS         — `# AUTO` line has overrides; marker is wrong.')
    print('  MANUAL_WITHOUT_CONTENT  — `# MANUAL` line has no overrides; marker is rot.')
    print('Cosmetic-only: marker text does not affect recompiler behavior.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
