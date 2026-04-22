#!/usr/bin/env python3
"""
cfg_exclude_range_audit — classify each `exclude_range X Y` directive
in cfg by whether the bytes really look like data.

For each range, decode the bytes as if they were code and run
validate_decoded_insns. If validation FAILS or decode stops early,
the range is "data-looking" (exclude is correct). If decode produces
a clean sequence of plausible instructions, the range is "code-looking"
(exclude might be incorrect — worth reviewing).

This is a heuristic, not a proof — some data sequences happen to
disassemble as valid code. But ranges that validate as code warrant
human review.

Output: table of (bank, lo, hi, size, classification, reason).
"""
import argparse
import pathlib
import re
import sys
from typing import List

REPO = pathlib.Path(__file__).resolve().parent.parent
PARENT = pathlib.Path('F:/Projects/SuperMarioWorldRecomp')
CFG_DIR = PARENT / 'recomp'
BANKS = (0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x07, 0x0c, 0x0d)

sys.path.insert(0, str(REPO / 'recompiler'))
from snes65816 import load_rom, lorom_offset, decode_insn, validate_decoded_insns  # noqa: E402


def audit_bank(rom, bank: int, verbose: bool) -> List[dict]:
    cp = CFG_DIR / f'bank{bank:02x}.cfg'
    if not cp.exists():
        return []
    ranges = []
    ex_re = re.compile(
        r'^\s*exclude_range\s+([0-9a-fA-F]{4,6})\s+([0-9a-fA-F]{4,6})'
    )
    for i, line in enumerate(cp.read_text(encoding='utf-8', errors='replace').splitlines()):
        m = ex_re.match(line)
        if not m:
            continue
        try:
            lo = int(m.group(1), 16) & 0xFFFF
            hi = int(m.group(2), 16) & 0xFFFF
        except ValueError:
            continue
        size = hi - lo + 1
        # Decode bytes from lo to hi, starting M=1, X=1, bank=bank.
        pc = lo
        m_flag, x_flag = 1, 1
        insns = []
        decode_ok = True
        while pc <= hi and len(insns) < 200:
            try:
                off = lorom_offset(bank, pc)
                insn = decode_insn(rom, off, pc, bank, m_flag, x_flag)
            except (AssertionError, IndexError):
                decode_ok = False
                break
            if insn is None:
                decode_ok = False
                break
            insns.append(insn)
            pc += insn.length
            if insn.mnem in ('RTS', 'RTL', 'RTI', 'JMP', 'BRA', 'BRL'):
                break
            if insn.mnem == 'REP':
                if insn.operand & 0x20: m_flag = 0
                if insn.operand & 0x10: x_flag = 0
            elif insn.mnem == 'SEP':
                if insn.operand & 0x20: m_flag = 1
                if insn.operand & 0x10: x_flag = 1
        if not insns:
            classification = 'DATA'
            reason = 'no insns decode from start'
        elif not decode_ok:
            classification = 'DATA'
            reason = f'decode failed after {len(insns)} insns'
        else:
            try:
                valid = validate_decoded_insns(insns, bank)
            except Exception:
                valid = False
            if valid and len(insns) >= 3:
                classification = 'CODE_LOOKING'
                reason = f'{len(insns)} insns validate as code'
            else:
                classification = 'DATA'
                reason = f'{len(insns)} insns; validation {valid}'
        ranges.append(dict(
            bank=bank, line=i, lo=lo, hi=hi, size=size,
            classification=classification, reason=reason,
            first_insn=insns[0].mnem if insns else '',
            first_addr=lo,
        ))
    return ranges


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument('--bank', type=lambda x: int(x, 16))
    p.add_argument('--list-code-looking', action='store_true')
    p.add_argument('--limit', type=int, default=0)
    args = p.parse_args()

    rom = load_rom(str(PARENT / 'smw.sfc'))
    banks = [args.bank] if args.bank is not None else list(BANKS)
    all_results = []
    for bank in banks:
        rs = audit_bank(rom, bank, verbose=args.list_code_looking)
        all_results.extend(rs)

    total = len(all_results)
    by_class = {}
    for r in all_results:
        by_class.setdefault(r['classification'], []).append(r)

    print(f'# exclude_range audit (total={total})')
    for cls, lst in sorted(by_class.items()):
        print(f'  {cls:<15}: {len(lst)}')

    if args.list_code_looking:
        code_list = by_class.get('CODE_LOOKING', [])
        if args.limit:
            code_list = code_list[:args.limit]
        print(f'\n## CODE_LOOKING ({len(code_list)} shown — candidate wrong excludes)')
        print(f'{"bank":<5} {"range":<15} {"size":<5} {"first_insn":<8} reason')
        for r in code_list:
            rg = f'${r["lo"]:04x}-${r["hi"]:04x}'
            print(f'  {r["bank"]:02x}    {rg:<15} {r["size"]:<5} {r["first_insn"]:<8} {r["reason"]}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
