"""snesrecomp.tools.v2_regen

Drive the v2 pipeline over every bank cfg in a SMW-style repo,
producing one C file per bank. Side-by-side with v1 — does NOT touch
src/gen/ or recomp/funcs.h.

Usage:
    python snesrecomp/tools/v2_regen.py --rom smw.sfc \
        --cfg-dir SuperMarioWorldRecomp/recomp \
        --out-dir SuperMarioWorldRecomp/src/gen_v2

For each `bankXX.cfg` under --cfg-dir:
    1. parse via cfg_loader.load_bank_cfg
    2. emit via emit_bank.emit_bank
    3. write to <out_dir>/smw_XX_v2.c

Exits 0 if every bank completed; non-zero otherwise. Per-bank failures
are caught and reported individually so a single bug doesn't block
the rest of the integration run.
"""

import argparse
import pathlib
import re
import sys
import traceback

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / 'recompiler'))

from snes65816 import load_rom  # noqa: E402
from v2.cfg_loader import load_bank_cfg  # noqa: E402
from v2.emit_bank import emit_bank  # noqa: E402


_BANK_CFG_RE = re.compile(r'bank([0-9a-fA-F]+)\.cfg$')


def main() -> int:
    p = argparse.ArgumentParser(description="v2 regen — emit one C file per bank cfg")
    p.add_argument('--rom', required=True, help='Path to SMW ROM file (.sfc)')
    p.add_argument('--cfg-dir', required=True,
                   help='Directory containing bankXX.cfg files')
    p.add_argument('--out-dir', required=True,
                   help='Output directory for emitted C files')
    args = p.parse_args()

    rom = load_rom(args.rom)
    cfg_dir = pathlib.Path(args.cfg_dir)
    out_dir = pathlib.Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    cfgs = sorted(cfg_dir.glob('bank*.cfg'))
    if not cfgs:
        print(f"v2_regen: no bank*.cfg under {cfg_dir}", file=sys.stderr)
        return 2

    total = len(cfgs)
    succeeded = 0
    failed = []

    for cfg_path in cfgs:
        m = _BANK_CFG_RE.search(cfg_path.name)
        if not m:
            print(f"v2_regen: skipping {cfg_path.name} (unrecognized name)", file=sys.stderr)
            continue
        bank = int(m.group(1), 16)
        out_path = out_dir / f'smw_{bank:02x}_v2.c'

        try:
            cfg = load_bank_cfg(str(cfg_path))
            if cfg.bank != bank:
                print(f"  {cfg_path.name}: bank field ${cfg.bank:02X} doesn't match filename ${bank:02X}; using filename")
            src = emit_bank(rom, bank=bank, entries=cfg.entries)
            out_path.write_text(src, encoding='utf-8')
            print(f"  OK    bank ${bank:02X}: {len(cfg.entries)} entries -> {out_path}")
            succeeded += 1
        except Exception as e:
            print(f"  FAIL  bank ${bank:02X}: {type(e).__name__}: {e}")
            traceback.print_exc()
            failed.append((bank, str(e)))

    print()
    print(f"v2_regen: {succeeded}/{total} banks emitted")
    if failed:
        print(f"failed banks:")
        for bank, msg in failed:
            print(f"  ${bank:02X}: {msg}")
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
