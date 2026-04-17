#!/usr/bin/env python3
"""Parse SMWDisX rammap.asm and hardware_registers.asm into a JSON symbol table.

Usage:
    python parse_smwdisx_symbols.py <path_to_SMWDisX_dir> [-o output.json]

Output format:
    {
        "ram": {"0x0072": "PlayerInAir", "0x0100": "GameMode", ...},
        "reg": {"0x2100": "INIDISP", "0x4200": "NMITIMEN", ...}
    }

Addresses are hex strings with 0x prefix, lowercase.
"""
import argparse
import json
import os
import re
import sys


def parse_rammap(path: str) -> dict:
    """Parse rammap.asm into {hex_addr_str: label_name}."""
    symbols = {}
    addr = 0
    with open(path, 'r') as f:
        for line in f:
            raw = line.strip()
            # Stop at AUDIO RAM / SPC700 / DSP sections — those overlap WRAM addresses
            if raw in ('; AUDIO RAM', '; DSP REGISTERS'):
                break
            line = raw
            if not line or line.startswith(';'):
                continue
            # ORG directive changes base address
            m = re.match(r'ORG\s+\$([0-9A-Fa-f]+)', line)
            if m:
                addr = int(m.group(1), 16)
                # SMWDisX uses $7E0000-based addresses for WRAM
                # Strip the $7E bank prefix to get WRAM offset
                if addr >= 0x7E0000 and addr < 0x7F0000:
                    addr -= 0x7E0000
                elif addr >= 0x7F0000 and addr < 0x800000:
                    addr -= 0x7E0000  # 7F bank is also WRAM (upper 64K)
                continue
            # Constants (! prefix) — skip
            if line.startswith('!'):
                continue
            # incsrc — skip
            if line.startswith('incsrc'):
                continue
            # Label definition: NAME: skip N
            m = re.match(r'([A-Za-z_][A-Za-z0-9_]*)\s*:\s*skip\s+(\d+)', line)
            if m:
                name = m.group(1)
                size = int(m.group(2))
                # Skip scratch names like _0, _1, etc.
                if not re.match(r'^_[0-9A-Fa-f]$', name):
                    symbols['0x%04x' % addr] = name
                addr += size
                continue
            # Label without skip (just a label)
            m = re.match(r'([A-Za-z_][A-Za-z0-9_]*)\s*:', line)
            if m:
                name = m.group(1)
                if not re.match(r'^_[0-9A-Fa-f]$', name):
                    symbols['0x%04x' % addr] = name
                continue
            # skip without label (padding)
            m = re.match(r'skip\s+(\d+)', line)
            if m:
                addr += int(m.group(1))
                continue
    return symbols


def parse_hwregs(path: str) -> dict:
    """Parse hardware_registers.asm into {hex_addr_str: label_name}."""
    symbols = {}
    addr = 0
    with open(path, 'r') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith(';'):
                continue
            m = re.match(r'ORG\s+\$([0-9A-Fa-f]+)', line)
            if m:
                addr = int(m.group(1), 16)
                continue
            if line.startswith('!') or line.startswith('incsrc'):
                continue
            m = re.match(r'(HW_[A-Za-z0-9_]+)\s*:\s*skip\s+(\d+)', line)
            if m:
                name = m.group(1)
                size = int(m.group(2))
                # Strip HW_ prefix for cleaner comments
                clean = name[3:] if name.startswith('HW_') else name
                symbols['0x%04x' % addr] = clean
                addr += size
                continue
            m = re.match(r'([A-Za-z_][A-Za-z0-9_]*)\s*:\s*skip\s+(\d+)', line)
            if m:
                name = m.group(1)
                size = int(m.group(2))
                symbols['0x%04x' % addr] = name
                addr += size
                continue
            m = re.match(r'skip\s+(\d+)', line)
            if m:
                addr += int(m.group(1))
                continue
    return symbols


def main():
    p = argparse.ArgumentParser(description='Parse SMWDisX symbols into JSON')
    p.add_argument('smwdisx_dir', help='Path to SMWDisX repository')
    p.add_argument('-o', '--output', default='smw_symbols.json',
                   help='Output JSON file (default: smw_symbols.json)')
    args = p.parse_args()

    rammap = os.path.join(args.smwdisx_dir, 'rammap.asm')
    hwregs = os.path.join(args.smwdisx_dir, 'hardware_registers.asm')

    if not os.path.isfile(rammap):
        print('Error: %s not found' % rammap, file=sys.stderr)
        sys.exit(1)

    result = {}
    result['ram'] = parse_rammap(rammap)
    print('Parsed %d RAM symbols' % len(result['ram']))

    if os.path.isfile(hwregs):
        result['reg'] = parse_hwregs(hwregs)
        print('Parsed %d register symbols' % len(result['reg']))
    else:
        print('Warning: %s not found, skipping register symbols' % hwregs)
        result['reg'] = {}

    with open(args.output, 'w') as f:
        json.dump(result, f, indent=2)
    print('Wrote %s' % args.output)


if __name__ == '__main__':
    main()
