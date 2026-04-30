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
from v2.codegen import set_name_resolver, take_unresolved_call_targets  # noqa: E402
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

    # First pass: load every cfg and build a global name resolver. This
    # lets cross-bank Call ops in the per-bank emit (second pass) resolve
    # to the friendly name the target's cfg declared via `func` or `name`.
    parsed: list[tuple[int, pathlib.Path, object]] = []
    name_map: dict[int, str] = {}
    # Collect every `name <addr> <friendly>` line across ALL cfgs grouped
    # by the address's owning bank. After cfg load, these get promoted to
    # emit entries on the OWNING bank — handles cross-bank label decls
    # (e.g. bank 01's `name 0086df` declares an entry that bank 00 must
    # emit). v1's auto-promote did this implicitly via JSL/JSR scanning.
    cross_bank_names: dict[int, list] = {}
    for cfg_path in cfgs:
        m = _BANK_CFG_RE.search(cfg_path.name)
        if not m:
            continue
        bank = int(m.group(1), 16)
        try:
            cfg = load_bank_cfg(str(cfg_path))
        except Exception as e:
            print(f"  PARSE-FAIL bank ${bank:02X}: {type(e).__name__}: {e}")
            continue
        parsed.append((bank, cfg_path, cfg))
        for entry in cfg.entries:
            if entry.name:
                name_map[(bank << 16) | (entry.start & 0xFFFF)] = entry.name
        for nd in cfg.names:
            addr = nd.addr_24 & 0xFFFFFF
            name_map[addr] = nd.name
            cross_bank_names.setdefault((addr >> 16) & 0xFF, []).append(nd)

    # Promote cross-bank `name` decls into target bank's emit entries.
    # Skip when the bank already has either (a) an entry at the same PC,
    # or (b) any entry with the same friendly name (handles cfg drift
    # where two banks point at slightly different addresses for the
    # same logical entry — v1's auto-promote picked one by JSL scan,
    # we pick the first-seen). Track friendly-name claims GLOBALLY: if
    # bank A already defines `Foo`, bank B can't also define one (else
    # the linker sees two definitions of `Foo`).
    from v2.emit_bank import BankEntry  # local import to avoid top-level cycle
    global_names: set[str] = set()
    for _bank, _cfg_path, cfg in parsed:
        for e in cfg.entries:
            if e.name:
                global_names.add(e.name)
    for bank, _cfg_path, cfg in parsed:
        existing_starts = {e.start & 0xFFFF for e in cfg.entries}
        existing_names = {e.name for e in cfg.entries if e.name}
        for nd in cross_bank_names.get(bank, []):
            local_pc = nd.addr_24 & 0xFFFF
            if local_pc in existing_starts:
                continue
            if nd.name in existing_names or nd.name in global_names:
                continue
            cfg.entries.append(BankEntry(name=nd.name, start=local_pc))
            existing_starts.add(local_pc)
            existing_names.add(nd.name)
            global_names.add(nd.name)

    set_name_resolver(name_map)

    total = len(parsed)
    succeeded = 0
    failed = []

    # Iterative emit + auto-promote loop. Each pass:
    #   1. emit every bank
    #   2. drain codegen's unresolved-Call-targets set (synthetic
    #      `bank_BB_AAAA` references whose target had no friendly name)
    #   3. for every unresolved target whose owning bank doesn't already
    #      have an entry there, add a synthetic-name BankEntry
    #   4. re-emit if any new entries were added; else done
    #
    # Mirrors v1's auto-promote, which discovered new function bodies by
    # following JSL/JSR targets during decode. v2 instead discovers them
    # post-emit, then re-emits affected banks.
    from v2.emit_bank import BankEntry  # local import again (already done above; harmless)

    max_passes = 8
    last_unresolved: set = set()
    for pass_idx in range(max_passes):
        # Clear any leftovers from prior session/process.
        take_unresolved_call_targets()
        succeeded = 0
        failed = []

        for bank, cfg_path, cfg in parsed:
            out_path = out_dir / f'smw_{bank:02x}_v2.c'
            try:
                if cfg.bank != bank:
                    print(f"  {cfg_path.name}: bank field ${cfg.bank:02X} doesn't match filename ${bank:02X}; using filename")
                src = emit_bank(rom, bank=bank, entries=cfg.entries)
                out_path.write_text(src, encoding='utf-8')
                if pass_idx == 0:
                    print(f"  OK    bank ${bank:02X}: {len(cfg.entries)} entries -> {out_path}")
                succeeded += 1
            except Exception as e:
                print(f"  FAIL  bank ${bank:02X}: {type(e).__name__}: {e}")
                traceback.print_exc()
                failed.append((bank, str(e)))

        unresolved = take_unresolved_call_targets()
        last_unresolved = unresolved
        if not unresolved:
            break

        added = 0
        # Bucket unresolved targets by owning bank, then dedupe against
        # each bank's existing entries (PC + friendly name).
        by_bank: dict[int, list[int]] = {}
        for addr in unresolved:
            by_bank.setdefault((addr >> 16) & 0xFF, []).append(addr & 0xFFFF)

        bank_index = {b: cfg for (b, _p, cfg) in parsed}
        for bank, pcs in by_bank.items():
            cfg = bank_index.get(bank)
            if cfg is None:
                # Cross-bank target whose owning bank has no cfg in this
                # repo; nothing to auto-promote, the symbol stays
                # unresolved (later passes will keep flagging it but the
                # set is per-pass so we exit).
                continue
            existing_starts = {e.start & 0xFFFF for e in cfg.entries}
            for pc in pcs:
                if pc in existing_starts:
                    continue
                synth_name = f"bank_{bank:02X}_{pc:04X}"
                cfg.entries.append(BankEntry(name=synth_name, start=pc))
                existing_starts.add(pc)
                added += 1

        if added == 0:
            break
        print(f"  auto-promote pass {pass_idx + 1}: added {added} entries; re-emitting")

    # Final pass: any still-unresolved Call targets after the last emit
    # belong to ROM banks not in the cfg set (e.g. data decoded as code
    # that produced a JSL into bank $24/$67/etc.). Emit one shared stub
    # file with empty bodies so the linker is happy. Real execution
    # paths shouldn't reach these; if they do, the stubs are no-ops.
    by_bank: dict[int, list[int]] = {}
    bank_set = {b for (b, _p, _c) in parsed}
    for addr in last_unresolved:
        bank = (addr >> 16) & 0xFF
        if bank in bank_set:
            continue
        by_bank.setdefault(bank, []).append(addr & 0xFFFF)
    if by_bank:
        stub_path = out_dir / 'unresolved_stubs_v2.c'
        lines = [
            '/* Auto-generated by snesrecomp v2 v2_regen. Do NOT hand-edit.',
            ' *',
            ' * Stub bodies for Call targets that resolved to a ROM bank not',
            ' * in the cfg set. These are typically data decoded as code',
            ' * (garbled JSL operands). Real execution paths should never',
            ' * reach them; the stubs exist solely so the linker resolves.',
            ' */',
            '',
            '#include "cpu_state.h"',
            '',
        ]
        for bank in sorted(by_bank):
            for pc in sorted(set(by_bank[bank])):
                lines.append(
                    f'void bank_{bank:02X}_{pc:04X}(CpuState *cpu) {{ (void)cpu; }}'
                )
        stub_path.write_text('\n'.join(lines) + '\n', encoding='utf-8')
        print(f"  emitted stubs for {sum(len(v) for v in by_bank.values())} cross-ROM-bank targets -> {stub_path}")

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
