#!/usr/bin/env python3
"""
Converts smw_04.c to use per-function RECOMP_BANK04 guards.
Idempotent: strips all existing RECOMP_BANK04 guards first, then re-adds.
Only func'd functions (those with `func` entries in bank04.cfg) get wrapped.
Skip'd functions and static data remain always-compiled.

Usage: python tools/recomp/add_bank04_guards.py
"""
import re

BANK_CFG  = 'tools/recomp/bank04.cfg'
ORACLE    = 'src/smw_04.c'
GUARD     = 'RECOMP_BANK04'

# --- Step 1: collect func'd names from bank04.cfg --------------------
func_names = set()
with open(BANK_CFG, 'r') as f:
    for line in f:
        m = re.match(r'^func\s+(\w+)', line)
        if m:
            func_names.add(m.group(1))

print(f"[guard-tool] func'd names: {len(func_names)}")

# --- Step 2: read smw_04.c -------------------------------------------
with open(ORACLE, 'r') as f:
    raw = f.readlines()

# --- Step 3: strip ALL existing RECOMP_BANK04 guard lines (idempotent)
stripped = []
for line in raw:
    s = line.strip()
    if s == f'#ifndef {GUARD}' or s in (f'#endif  // {GUARD}', f'#endif // {GUARD}'):
        continue
    stripped.append(line)

print(f"[guard-tool] lines after stripping: {len(stripped)}")

# --- Step 4: re-add per-function guards ------------------------------
FUNC_START_RE = re.compile(r'^(?:static\s+)?(?:void|uint8|uint16|uint32|int)\s+(\w+)\s*\(')

result      = []
i           = 0
guarded     = 0
brace_depth = 0  # track top-level context

while i < len(stripped):
    line = stripped[i]

    if brace_depth == 0:
        m = FUNC_START_RE.match(line)
        if m and m.group(1) in func_names:
            result.append(f'#ifndef {GUARD}\n')
            guarded += 1
            result.append(line)
            brace_depth += line.count('{') - line.count('}')
            i += 1
            while i < len(stripped):
                fline = stripped[i]
                result.append(fline)
                brace_depth += fline.count('{') - fline.count('}')
                i += 1
                if brace_depth == 0:
                    result.append(f'#endif  // {GUARD}\n')
                    break
            continue

    brace_depth += line.count('{') - line.count('}')
    result.append(line)
    i += 1

print(f"[guard-tool] guarded {guarded} functions")

# --- Step 5: write back ----------------------------------------------
with open(ORACLE, 'w', newline='\n') as f:
    f.writelines(result)

print(f"[guard-tool] done: {ORACLE}")
