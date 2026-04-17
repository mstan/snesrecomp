#!/usr/bin/env python3
"""Promote the first N skip entries in bank01.cfg to func, using addresses from name entries."""
import sys

N = int(sys.argv[1]) if len(sys.argv) > 1 else 50
CFG = 'tools/recomp/bank01.cfg'

with open(CFG) as f:
    lines = f.readlines()

promoted = 0
result = []
i = 0
while i < len(lines):
    line = lines[i]
    stripped = line.strip()
    if stripped.startswith('skip ') and not line.rstrip().endswith('# nopromote') and promoted < N:
        func_name = stripped[5:].strip()
        # Next line should be the companion name entry
        if i + 1 < len(lines) and lines[i+1].strip().startswith('name '):
            name_parts = lines[i+1].strip().split()
            # name 01XXXX FuncName sig:...
            addr_full = name_parts[1]          # e.g. '018008'
            addr_hex  = addr_full[2:]          # strip bank prefix '01' → '8008'
            sig_tok   = next((p for p in name_parts[3:] if p.startswith('sig:')), None)
            if sig_tok:
                result.append(f'func {func_name} {addr_hex} {sig_tok}\n')
            else:
                result.append(f'func {func_name} {addr_hex}\n')
            promoted += 1
        else:
            result.append(line)
    else:
        result.append(line)
    i += 1

print(f'Promoted {promoted} functions to func')
with open(CFG, 'w', newline='\n') as f:
    f.writelines(result)
