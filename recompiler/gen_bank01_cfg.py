#!/usr/bin/env python3
"""
Parse smw_01.c and generate bank01.cfg with all functions as `skip` + `name` entries.
- `skip FuncName` prevents gen output for that function
- `name 01xxxx FuncName` registers the address→name mapping so promoted functions
  can resolve branches/tail-calls to still-skipped siblings.
- Sig is omitted from name entries; funcs.h is auto-loaded by recomp.py.
"""
import re

ORACLE = 'src/smw_01.c'
OUT    = 'tools/recomp/bank01.cfg'

# Match function definition lines like:
#   void FuncName(uint8 k) {  // 01abcd
#   uint8 FuncName(uint8 k) {  // 01abcd
#   bool FuncName() {  // 01abcd
FUNC_RE = re.compile(
    r'^(?:static\s+)?'
    r'([\w\s\*]+?)\s+'         # return type (possibly multi-word like "const uint8 *")
    r'(\w+)\s*\(([^)]*)\)\s*\{.*//\s*01([0-9a-fA-F]{4})'
)

with open(ORACLE, 'r') as f:
    lines = f.readlines()

entries = []
for line in lines:
    m = FUNC_RE.match(line)
    if not m:
        continue
    ret_raw = m.group(1).strip()
    name    = m.group(2)
    params  = m.group(3).strip()
    addr    = m.group(4).upper()

    # Normalise return type for sig string
    ret = ret_raw.replace(' ', '_').replace('*', 'p')
    if ret in ('static_void', 'static_uint8'):
        ret = ret.replace('static_', '')

    # Build param sig tokens
    param_toks = []
    if params and params != 'void':
        for p in params.split(','):
            p = p.strip()
            if p:
                param_toks.append('_'.join(p.split()))

    if param_toks:
        sig = f'{ret}({",".join(param_toks)})'
    else:
        sig = f'{ret}()'

    entries.append((name, addr, sig))

print(f"Found {len(entries)} functions")

with open(OUT, 'w', newline='\n') as f:
    f.write('bank = 01\n\n')
    f.write(f'# bank01.cfg — {len(entries)} functions, all start as skip.\n')
    f.write('# Promote to func as the recompiler is validated.\n')
    f.write('# name entries register address->name so promoted funcs can resolve tail calls.\n\n')

    for name, addr, sig in entries:
        f.write(f'skip {name}\n')
        f.write(f'name 01{addr} {name} sig:{sig}\n')

    f.write('\nverbatim_start\n')
    f.write('#include "../common_rtl.h"\n')
    f.write('#include "../funcs.h"\n')
    f.write('#include "../smw_rtl.h"\n')
    f.write('#include "../variables.h"\n')
    f.write('#include "../consts.h"\n')
    f.write('#include "../../assets/smw_assets.h"\n')
    f.write('verbatim_end\n')

print(f"Wrote {OUT}")
