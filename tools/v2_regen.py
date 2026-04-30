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
from v2.decoder import classify_dispatch_helper, decode_function  # noqa: E402
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

    # Auto-detect dispatch helpers across ALL banks: scan every
    # cfg-declared function for JSL/JML targets, classify each by
    # subroutine signature (PLA/PLY + indirect JMP). Result: a
    # global {target_addr_24 -> 'short'|'long'} map passed into
    # emit_bank so the decoder treats bytes-after-JSL as a TABLE
    # instead of garbage instructions.
    print()
    print("Auto-detecting JSL dispatch helpers...")
    dispatch_helpers: dict = {}
    jsl_targets: set = set()
    for bank, _cfg_path, cfg in parsed:
        for entry in cfg.entries:
            try:
                graph = decode_function(rom, bank, entry.start,
                                        entry_m=entry.entry_m,
                                        entry_x=entry.entry_x,
                                        end=entry.end)
            except Exception:
                continue
            for di in graph.insns.values():
                ins = di.insn
                # JSL or JML (JMP LONG)
                if ins.mnem == 'JSL':
                    jsl_targets.add(ins.operand & 0xFFFFFF)
                elif ins.mnem == 'JMP' and ins.length == 4:
                    jsl_targets.add(ins.operand & 0xFFFFFF)
    classified = {'short': 0, 'long': 0}
    for tgt in jsl_targets:
        tbank = (tgt >> 16) & 0xFF
        taddr = tgt & 0xFFFF
        kind = classify_dispatch_helper(rom, tbank, taddr)
        if kind:
            dispatch_helpers[tgt] = kind
            classified[kind] += 1
    print(f"  detected {classified['short']} short + {classified['long']} long dispatch helpers "
          f"(scanned {len(jsl_targets)} JSL/JML targets)")

    # Pre-pass: discover (callee_addr_24, m, x) variants needed.
    #
    # The (M, X) flags affect 65816 instruction byte counts (LDA #imm
    # is 3 bytes when M=0, 2 bytes when M=1; same shape for LDX/LDY +
    # X). A function reached from contexts with different (m, x)
    # decodes to a literally different instruction stream and must be
    # emitted as a separate C body. This pre-pass scans every cfg
    # entry's Call ops and collects all per-(m, x) variants that
    # caller code asks for, so later emit can synthesise BankEntries
    # with the right entry_m/entry_x — instead of letting auto-promote
    # default everything to (1, 1) and emit a single body that's wrong
    # for half its callers.
    #
    # Without this pre-pass the FetchByte class of bug recurs: cfg
    # declares `func DecompressTo_FetchByte b983` (entry default 1,1),
    # decoder emits one M1X1 body, but DecompressTo callers run x=0
    # and the M1X1 body misdecodes LDX #$8000 as LDX #$00 + falling
    # opcode bytes.
    print()
    print("Discovering per-(m,x) variants (fixed-point)...")
    # variants: dict[addr_24 -> set[(m, x)]]
    #
    # Iterate to a fixed point. Each pass decodes every (entry_addr, m, x)
    # we haven't decoded yet, scans its Call ops for callee (m, x)
    # demands, and adds any new (callee_addr, m, x) tuples to the queue.
    # Without iteration, Call ops INSIDE auto-promoted variants would
    # not contribute to discovery — so e.g. cfg declares Foo at M1X1,
    # we discover Bar needs M0X0, but Bar(M0X0)'s body's Calls into Baz
    # at M0X0 stay invisible until Bar(M0X0) is itself decoded.
    variants: dict[int, set] = {}
    # Seed with cfg-default entries.
    queue: list[tuple[int, int, int, int, "Optional[int]"]] = []  # (bank, start, m, x, end)
    addr_to_end: dict[int, "Optional[int]"] = {}
    addr_to_bank: dict[int, int] = {}
    for bank, _cfg_path, cfg in parsed:
        for entry in cfg.entries:
            addr = (bank << 16) | (entry.start & 0xFFFF)
            addr_to_end[addr] = entry.end
            addr_to_bank[addr] = bank
            mx = (entry.entry_m & 1, entry.entry_x & 1)
            if mx not in variants.setdefault(addr, set()):
                variants[addr].add(mx)
                queue.append((bank, entry.start, mx[0], mx[1], entry.end))

    decoded: set = set()  # (addr, m, x) already decoded for variant discovery
    iterations = 0
    # Cap is generous: ~2000 entries × 4 (m, x) variants = 8000 max
    # decode budget; double that for headroom.
    while queue:
        iterations += 1
        if iterations > 100000:
            print(f"  variant discovery loop overran 100000 iterations — bailing")
            break
        bank, start, em, ex, end = queue.pop()
        addr = (bank << 16) | (start & 0xFFFF)
        if (addr, em, ex) in decoded:
            continue
        decoded.add((addr, em, ex))
        try:
            graph = decode_function(rom, bank, start,
                                    entry_m=em, entry_x=ex,
                                    end=end,
                                    dispatch_helpers=dispatch_helpers)
        except Exception:
            continue
        for di in graph.insns.values():
            ins = di.insn
            # JSR ABS (length 3) and JSL (length 4) — both produce a
            # Call IR. Indirect-X JSR has no static target.
            if ins.mnem == 'JSR' and ins.length == 3:
                src_bank = (ins.addr >> 16) & 0xFF
                target = ((src_bank << 16) | (ins.operand & 0xFFFF))
            elif ins.mnem == 'JSL':
                target = ins.operand & 0xFFFFFF
            elif ins.mnem == 'JMP' and ins.length == 4:
                # JML is a tail-call equivalent; treat similarly.
                target = ins.operand & 0xFFFFFF
            else:
                # Dispatch tables on this insn still demand variants.
                target = None
            if target is not None:
                em2 = ins.m_flag & 1
                ex2 = ins.x_flag & 1
                if (em2, ex2) not in variants.setdefault(target, set()):
                    variants[target].add((em2, ex2))
                    # Queue decode of this variant if it's an in-cfg target.
                    if target in addr_to_end:
                        tb = addr_to_bank[target]
                        ts = target & 0xFFFF
                        queue.append((tb, ts, em2, ex2, addr_to_end[target]))
            # Dispatch tables: each entry is also a callee with the
            # dispatcher's (m, x).
            for d_target in getattr(ins, 'dispatch_entries', None) or []:
                if d_target == 0:
                    continue
                if getattr(ins, 'dispatch_kind', 'short') == 'long':
                    d_addr = d_target & 0xFFFFFF
                else:
                    d_addr = ((ins.addr >> 16) & 0xFF) << 16 | (d_target & 0xFFFF)
                em2 = ins.m_flag & 1
                ex2 = ins.x_flag & 1
                if (em2, ex2) not in variants.setdefault(d_addr, set()):
                    variants[d_addr].add((em2, ex2))
                    if d_addr in addr_to_end:
                        tb = addr_to_bank[d_addr]
                        ts = d_addr & 0xFFFF
                        queue.append((tb, ts, em2, ex2, addr_to_end[d_addr]))
    multi_count = sum(1 for v in variants.values() if len(v) > 1)
    print(f"  variants for {len(variants)} unique callee targets; "
          f"{multi_count} multi-(m,x); decoded {len(decoded)} (addr, m, x) tuples")

    # Apply per-(m,x) variants to existing cfg entries: for each cfg
    # entry whose target address has more than its declared (m, x)
    # variant, clone the entry with each additional (m, x). The
    # cfg-declared variant remains the "canonical" one (the alias in
    # emit_bank points to it); other variants exist only as gen
    # bodies referenced by mangled-name Call ops.
    for bank, _cfg_path, cfg in parsed:
        new_entries: list = []
        seen_keys: set = set()
        for entry in cfg.entries:
            addr = (bank << 16) | (entry.start & 0xFFFF)
            decl_mx = (entry.entry_m & 1, entry.entry_x & 1)
            seen_keys.add((addr, decl_mx))
            new_entries.append(entry)
            extras = variants.get(addr, set()) - {decl_mx}
            for em, ex in sorted(extras):
                key = (addr, (em, ex))
                if key in seen_keys:
                    continue
                seen_keys.add(key)
                # Clone with the extra (m,x). Same name and end:; the
                # variant suffix is applied at emit time so two cfg
                # entries with the same `name` resolve to two distinct
                # C symbols (`<name>_M1X0`, `<name>_M1X1`).
                new_entries.append(BankEntry(
                    name=entry.name,
                    start=entry.start,
                    end=entry.end,
                    entry_m=em,
                    entry_x=ex,
                ))
        cfg.entries = new_entries

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
                src = emit_bank(rom, bank=bank, entries=cfg.entries,
                                dispatch_helpers=dispatch_helpers)
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
        # Bucket Call-demand (addr, m, x) tuples by owning bank.
        # Codegen now records ALL Call demands (resolved + synthetic),
        # so this includes cfg-named targets whose new (m, x) variant
        # hasn't been emitted yet.
        by_bank: dict[int, list[tuple[int, int, int]]] = {}
        for addr, em, ex in unresolved:
            by_bank.setdefault((addr >> 16) & 0xFF, []).append(
                (addr & 0xFFFF, em, ex)
            )

        bank_index = {b: cfg for (b, _p, cfg) in parsed}
        for bank, items in by_bank.items():
            cfg = bank_index.get(bank)
            if cfg is None:
                # Cross-bank target whose owning bank has no cfg in this
                # repo; nothing to auto-promote, the symbol stays
                # unresolved (later passes will keep flagging it but the
                # set is per-pass so we exit).
                continue
            # Existing-entry index by start PC so we can clone an
            # already-named cfg entry at a new (m, x). Without this,
            # cfg-named targets would only ever emit at their cfg-
            # default (m, x) and gen would reference unresolved
            # variants like UpdateEntirePalette_M0X0.
            existing_keys: set = {
                (e.start & 0xFFFF, e.entry_m & 1, e.entry_x & 1)
                for e in cfg.entries
            }
            entries_by_pc: dict[int, "BankEntry"] = {}
            for e in cfg.entries:
                entries_by_pc.setdefault(e.start & 0xFFFF, e)
            for pc, em, ex in items:
                key = (pc, em, ex)
                if key in existing_keys:
                    continue
                # If there's an existing entry at this PC (any (m, x)),
                # clone its name + end so the new variant emits as
                # `<name>_M{em}X{ex}`. Otherwise synthesize.
                base_entry = entries_by_pc.get(pc)
                if base_entry is not None:
                    cfg.entries.append(BankEntry(
                        name=base_entry.name,
                        start=pc,
                        end=base_entry.end,
                        entry_m=em,
                        entry_x=ex,
                    ))
                else:
                    synth_name = f"bank_{bank:02X}_{pc:04X}"
                    new_entry = BankEntry(
                        name=synth_name, start=pc,
                        entry_m=em, entry_x=ex,
                    )
                    cfg.entries.append(new_entry)
                    entries_by_pc[pc] = new_entry
                existing_keys.add(key)
                added += 1

        if added == 0:
            break
        print(f"  auto-promote pass {pass_idx + 1}: added {added} entries; re-emitting")

    # Final pass: any still-unresolved Call targets after the last emit
    # belong to ROM banks not in the cfg set (e.g. data decoded as code
    # that produced a JSL into bank $24/$67/etc.). Emit one shared stub
    # file with empty bodies so the linker is happy. Real execution
    # paths shouldn't reach these; if they do, the stubs are no-ops.
    by_bank: dict[int, set] = {}
    bank_set = {b for (b, _p, _c) in parsed}
    for addr, em, ex in last_unresolved:
        bank = (addr >> 16) & 0xFF
        if bank in bank_set:
            continue
        by_bank.setdefault(bank, set()).add((addr & 0xFFFF, em & 1, ex & 1))
    if by_bank:
        stub_path = out_dir / 'unresolved_stubs_v2.c'
        lines = [
            '/* Auto-generated by snesrecomp v2 v2_regen. Do NOT hand-edit.',
            ' *',
            ' * Stub bodies for Call targets that resolved to a ROM bank not',
            ' * in the cfg set. These are typically data decoded as code',
            ' * (garbled JSL operands). Real execution paths should never',
            ' * reach them; the stubs exist solely so the linker resolves.',
            ' * One stub per (target, m, x) variant requested by the gen.',
            ' */',
            '',
            '#include "cpu_state.h"',
            '',
        ]
        total_stubs = 0
        for bank in sorted(by_bank):
            for pc, em, ex in sorted(by_bank[bank]):
                lines.append(
                    f'void bank_{bank:02X}_{pc:04X}_M{em}X{ex}(CpuState *cpu) {{ (void)cpu; }}'
                )
                total_stubs += 1
        stub_path.write_text('\n'.join(lines) + '\n', encoding='utf-8')
        print(f"  emitted stubs for {total_stubs} cross-ROM-bank (target, m, x) variants -> {stub_path}")

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
