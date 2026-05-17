"""snesrecomp.tools.v2_regen

Drive the v2 pipeline over every bank cfg in a SMW-style repo,
producing one C file per bank into the single active generated-code
directory.

Usage:
    python snesrecomp/tools/v2_regen.py --rom smw.sfc \
        --cfg-dir SuperMarioWorldRecomp/recomp \
        --out-dir SuperMarioWorldRecomp/src/gen

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
from v2.codegen import (  # noqa: E402
    set_name_resolver,
    set_rom_size,
    take_rejected_call_targets,
    take_unresolved_call_targets,
    take_unresolved_goto_targets,
)
from v2.decoder import (  # noqa: E402
    classify_dispatch_helper, decode_function, analyze_function_exit_mx,
)
from v2.emit_bank import emit_bank  # noqa: E402
from v2.wrapper_autoroute import detect_and_route as autoroute_wrappers, format_fix_summary  # noqa: E402
from v2.tail_call_autoroute import (  # noqa: E402
    detect_and_route as autoroute_tail_calls,
    format_fix_summary as format_tail_call_summary,
)
from v2.exit_mx_autoroute import (  # noqa: E402
    detect_and_route as autoroute_exit_mx,
    format_fix_summary as format_exit_mx_summary,
)


_BANK_CFG_RE = re.compile(r'bank([0-9a-fA-F]+)\.cfg$')


# Stub-lint markers. Any emitted C line containing one of these strings
# is a recompiler stub — a code path that produced "do nothing" / "trap
# and return" placeholder output instead of real behavior. Hard-rule:
# the recompiler MUST emit no stubs. Each marker corresponds to a real
# code path in v2 that ducks out of emit; the lint exists so that the
# moment a new stub appears, the build fails loudly instead of letting
# the runtime quietly do nothing.
#
# Adding a new entry here is a one-line cost. Removing one requires
# closing the corresponding recompiler-level gap so the emit never
# produces the string in the first place.
_STUB_MARKERS = (
    'IndirectGoto: target',                  # codegen._emit_indirect_goto
    'IndirectGoto: dispatch table',          # emit_function indirect-goto fallthrough
    'Call indirect SUPPRESSED',              # codegen Call with suppressed table
    'Call: target unknown',                  # codegen Call with no target
    'unresolvable cross-fn goto',            # emit_function unresolved-goto trap
    'cpu_trace_unresolved_goto_trap',        # trap-fn call (any per-bank file)
    'cpu_trace_unresolved_stub_trap',        # trap-fn call (unresolved_stubs_v2.c)
)


def _lint_stubs(out_dir: pathlib.Path) -> list[tuple[str, int, str, str]]:
    """Scan every emitted .c file in out_dir for stub markers.

    Returns a list of (path, line_no, marker, line_text) tuples. Empty
    list means clean. Caller fails the build if non-empty.
    """
    hits: list[tuple[str, int, str, str]] = []
    for p in sorted(out_dir.glob('*.c')):
        try:
            with p.open('r', encoding='utf-8', errors='replace') as f:
                for ln, raw in enumerate(f, start=1):
                    for marker in _STUB_MARKERS:
                        if marker in raw:
                            hits.append((str(p), ln, marker, raw.rstrip('\n')))
                            break
        except OSError as e:
            print(f"  lint: failed to read {p}: {e}", file=sys.stderr)
    return hits


def main() -> int:
    p = argparse.ArgumentParser(description="v2 regen — emit one C file per bank cfg")
    p.add_argument('--rom', required=True, help='Path to game ROM file (.sfc)')
    p.add_argument('--cfg-dir', required=True,
                   help='Directory containing bankXX.cfg files')
    p.add_argument('--out-dir', required=True,
                   help='Output directory for emitted C files')
    p.add_argument('--prefix', default='smw',
                   help='Filename prefix for emitted bank files '
                        '(e.g. `smw` -> smw_00_v2.c; default: smw)')
    args = p.parse_args()

    rom = load_rom(args.rom)
    set_rom_size(len(rom))
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

    # Expand `auto_vectors` cfg directive: read the SNES interrupt-
    # vector table at ROM offset 0x7FE0-0x7FFF (LoROM mirror of
    # $00:FFE0-FFFF) and auto-seed I_RESET / I_NMI / I_IRQ entries
    # at the dereferenced PCs in bank 0's cfg. Lets a fresh game
    # project's bank00.cfg ship with one line instead of hand-decoded
    # vectors. Skip $0000 / $FFFF placeholder slots; skip duplicates
    # against existing cfg entries; warn if requested in a non-bank-0
    # cfg.
    from v2.emit_bank import BankEntry  # local import to avoid top-level cycle
    for bank, _cfg_path, cfg in parsed:
        if not cfg.auto_vectors:
            continue
        if bank != 0:
            print(f"  WARN: auto_vectors in bank ${bank:02X}.cfg ignored "
                  f"(vector table lives in bank $00)")
            continue
        rom_off = 0x7FE0
        if rom_off + 32 > len(rom):
            print("  WARN: auto_vectors: ROM too small for vector table")
            continue
        def _vec(slot_off):
            return (rom[rom_off + slot_off + 1] << 8) | rom[rom_off + slot_off]
        # Native NMI ($FFEA), native IRQ ($FFEE), emulation RESET
        # ($FFFC) are the three the framework's smw_rtl-style host
        # orchestration calls. Native is preferred over emulation for
        # NMI/IRQ — after I_RESET sets up native mode, steady-state
        # interrupts use the native slots.
        seed = [
            ('I_RESET', _vec(0x1C)),   # $FFFC emulation reset
            ('I_NMI',   _vec(0x0A)),   # $FFEA native NMI
            ('I_IRQ',   _vec(0x0E)),   # $FFEE native IRQ
        ]
        existing_starts = {e.start & 0xFFFF for e in cfg.entries}
        existing_names = {e.name for e in cfg.entries if e.name}
        added = []
        for name, pc in seed:
            if pc in (0x0000, 0xFFFF):
                continue
            if pc in existing_starts:
                continue
            if name in existing_names:
                continue
            cfg.entries.append(BankEntry(name=name, start=pc))
            name_map[(bank << 16) | pc] = name
            existing_starts.add(pc)
            existing_names.add(name)
            added.append((name, pc))
        if added:
            print(f"  auto_vectors: bank ${bank:02X}.cfg seeded "
                  + ", ".join(f"{n}=${pc:04X}" for n, pc in added))

    # Auto-route SMW PHB/PHK/PLB/JSR/PLB/RTL wrapper-bypass cfg aliases.
    # Class fix for the bug where cross-bank `name <wrapper_pc> <fn>` +
    # `name <body_pc> <fn>` (same `<fn>`) routes cross-bank JSL callers
    # past the wrapper, leaving DB at the caller's bank. See
    # `recompiler/v2/wrapper_autoroute.py` for the full diagnosis.
    print()
    print("Auto-routing SMW DB-transition wrappers...")
    wrapper_fixes = autoroute_wrappers(parsed, name_map, cross_bank_names, rom)
    print(format_fix_summary(wrapper_fixes))

    # Auto-detect tail-call fallthrough cfg sites. Pattern: cfg `func A
    # end:<pc>` whose <pc> is also the start of cfg `func B`, AND A's
    # last decoded instruction is a non-terminal that falls through to
    # exactly <pc>. emit_function would otherwise emit an unresolvable
    # goto at the boundary. See `recompiler/v2/tail_call_autoroute.py`.
    print()
    print("Auto-detecting tail-call fallthrough sites...")
    tail_call_fixes = autoroute_tail_calls(parsed, rom)
    print(format_tail_call_summary(tail_call_fixes))

    # Auto-detect dispatch helpers BEFORE exit-(M, X) autoroute. The
    # autoroute decoder needs `dispatch_helpers` to recognise SMW's
    # `JSL <helper>; <data table>` pattern as a dispatch terminator —
    # otherwise the bytes after the JSL are decoded as garbage
    # instructions, the function ends with no RTS/RTL/RTI, and
    # `analyze_function_exit_mx` returns ambiguous. The non-leaf
    # auto-router needs this signal to route exits for dispatch-
    # terminator functions like `BufferScrollingTiles_Layer1_Init`.
    # (Detection is identical to the existing block; it just moved up.)
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

    # Auto-detect leaf-function exit-(M, X) state mutations. Pattern:
    # cfg `func F` whose decoded body has NO JSR/JSL inside AND whose
    # RTS/RTL terminators all exit with the same (M, X) != entry (M, X)
    # — typically a small SEP/REP-then-RTS leaf. The decoder otherwise
    # assumes callees preserve (M, X), miscoding callers' post-call
    # operand widths. See `recompiler/v2/exit_mx_autoroute.py` for why
    # this is leaf-only (the unrestricted fixpoint version regressed
    # GraphicsDecompress on 2026-05-03 and was reverted to opt-in).
    print()
    print("Auto-detecting leaf-function exit-(M, X) mutations...")
    exit_mx_fixes = autoroute_exit_mx(parsed, rom,
                                      dispatch_helpers=dispatch_helpers)
    print(format_exit_mx_summary(exit_mx_fixes))

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

    # NOTE: dispatch_helpers was discovered earlier (above
    # autoroute_exit_mx) so the M/X exit-state auto-router can see
    # dispatch terminators. Re-use the same map for variant discovery
    # + per-bank emit below.

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
    # Aggregate cfg `data_region` directives across all banks. Passed
    # into decode_function on every variant-discovery + emit decode so
    # the dispatch-table reader and any future code-data classifier
    # has a single source of truth.
    all_data_regions: list = []
    for _bank, _cfg_path, _cfg in parsed:
        if _cfg.data_regions:
            all_data_regions.extend(_cfg.data_regions)

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
                                    dispatch_helpers=dispatch_helpers,
                                    data_regions=all_data_regions or None)
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
                # Defensive: if the static call target lands inside a
                # cfg data_region, the bytes there can't be a real
                # routine. Don't promote a variant for it. The
                # dispatch-table reader has the same check; this
                # second-line catches direct JSR/JSL into data.
                tbank_for_filter = (target >> 16) & 0xFF
                tpc_for_filter = target & 0xFFFF
                if all_data_regions and any(
                        (b & 0xFF) == tbank_for_filter and
                        (s & 0xFFFF) <= tpc_for_filter < (e & 0xFFFF)
                        for (b, s, e) in all_data_regions):
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

    # ── Callee-exit-(m,x) map from cfg `exit_mx:m,x` directives ─────
    #
    # Narrow opt-in: cfg lines may carry `exit_mx:M,X` to annotate a
    # function's exit (m, x) state. Used by decoder._labeled_successors
    # to set the resume (m, x) after JSR/JSL, instead of assuming the
    # callee preserves m/x. Required when the callee internally does
    # SEP/REP without restoring before its RTS — e.g. SMW $00:F465
    # starts with SEP #$20 (m=1) and never resets, so callers in m=0
    # would otherwise misdecode operand widths after the JSR.
    #
    # Earlier version (2026-05-03 morning) tried an automatic fixpoint
    # over EVERY decoded (addr, m, x) variant. Worked for the slope-
    # bug case but introduced regressions elsewhere (GraphicsDecompress
    # entered an infinite loop) — the analyzer's exit-(m,x) inference
    # was apparently wrong for some functions where intermediate
    # callee_exit_mx values during the fixpoint produced unreachable-
    # path artefacts that biased the analyzer. Reverted to opt-in until
    # we have a more principled inference (e.g. CFG-aware path
    # analysis, or per-edge propagation that doesn't rely on the same
    # decoder used for emit).
    callee_exit_mx: dict = {}
    cfg_exit_mx_count = 0
    declared_exit_mx: dict = {}  # (bank, addr16) -> (m, x)
    # Collect from `exit_mx_at <bankaddr16> <m> <x>` cfg directives
    # across all banks. This is the standalone form — independent of
    # any `func` entry, so callees discovered only via auto-promote
    # (e.g. $00:F461 — reached via JSR but with no own `func` line)
    # can still carry the annotation.
    for bank, _cfg_path, cfg in parsed:
        for (b_id, addr16, m_val, x_val) in cfg.exit_mx_at:
            declared_exit_mx[(b_id & 0xFF, addr16 & 0xFFFF)] = (m_val, x_val)
    # Broadcast each declared exit_mx to ALL (m, x) variants at the
    # target address. Variants are the discovered set in `variants`.
    for (b_id, addr16), (ex_m, ex_xf) in declared_exit_mx.items():
        target_pc24 = (b_id << 16) | addr16
        mx_set = variants.get(target_pc24)
        if mx_set:
            for em, ex2 in mx_set:
                callee_exit_mx[(target_pc24, em, ex2)] = (ex_m, ex_xf)
                cfg_exit_mx_count += 1
        else:
            # No discovered variants — apply to cfg-default (1, 1).
            callee_exit_mx[(target_pc24, 1, 1)] = (ex_m, ex_xf)
            cfg_exit_mx_count += 1

    # Per-variant exit_mx_at: populated by the auto-router with one
    # (entry_m, entry_x) → (exit_m, exit_x) tuple per mutating variant.
    # Per-variant entries OVERRIDE the broadcast 4-tuple at the same
    # (target, em, ex) key — but cfg-declared 4-tuples are seeded by
    # the auto-router itself before its own analysis runs, so hand-
    # written hints stay authoritative.
    per_variant_count = 0
    for bank, _cfg_path, cfg in parsed:
        for (b_id, addr16, em_in, ex_in, ex_m, ex_xf) in \
                cfg.exit_mx_at_per_variant:
            target_pc24 = ((b_id & 0xFF) << 16) | (addr16 & 0xFFFF)
            callee_exit_mx[(target_pc24, em_in & 1, ex_in & 1)] = (
                ex_m & 1, ex_xf & 1)
            per_variant_count += 1

    if cfg_exit_mx_count or per_variant_count:
        print()
        print(f"Loaded {cfg_exit_mx_count} cfg `exit_mx_at` broadcast "
              f"annotations ({len(declared_exit_mx)} unique targets); "
              f"{per_variant_count} per-variant overrides")

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

    def _autopromote_targets(parsed_repo, demands: set, *, source_kind: str) -> int:
        """Add BankEntry records for any (addr, m, x) demand tuple not
        already represented in the bank's cfg. Shared between Call-target
        and Goto-target auto-promotion. Returns the count of newly-added
        entries.

        `source_kind` is "call" or "goto" — only used for logging context;
        promotion logic is identical (same bucket-and-merge shape).
        """
        if not demands:
            return 0
        by_bank: dict[int, list[tuple[int, int, int]]] = {}
        for addr, em, ex in demands:
            tbank = (addr >> 16) & 0xFF
            tpc = addr & 0xFFFF
            # Refuse to synthesize a function entry inside a cfg
            # data_region. That range was declared as data; turning
            # bytes into a callable handler defeats the directive.
            if all_data_regions and any(
                    (b & 0xFF) == tbank and (s & 0xFFFF) <= tpc < (e & 0xFFFF)
                    for (b, s, e) in all_data_regions):
                continue
            by_bank.setdefault(tbank, []).append((tpc, em, ex))
        bank_index = {b: cfg for (b, _p, cfg) in parsed_repo}
        added_local = 0
        for bank, items in by_bank.items():
            cfg = bank_index.get(bank)
            if cfg is None:
                # Cross-bank target whose owning bank has no cfg in this
                # repo. For Calls: stays unresolved (final-pass stubs).
                # For Gotos: the tail-call site references a
                # bank_BB_AAAA_M*X* symbol that won't be defined here;
                # the same final-pass stub machinery covers it.
                continue
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
                added_local += 1
        return added_local

    # Bumped 2026-05-03: with the new callee-exit-(m,x) propagation,
    # decoder discovers more variants in transitive callees. 8 passes
    # leaves ~239 unresolved externals; 24 converges in practice.
    max_passes = 24
    last_unresolved: set = set()
    for pass_idx in range(max_passes):
        # Clear any leftovers from prior session/process.
        take_unresolved_call_targets()
        # take_unresolved_goto_targets() retired 2026-05-02 — goto
        # targets are now inlined into source functions by the decoder
        # (see decoder._labeled_successors), not auto-promoted.
        succeeded = 0
        failed = []

        # Aggregate suppressed JSR (abs,X) sites across the build for
        # the cfg-required-dispatch-or-kill report. Each emit_bank call
        # appends the bank's suppressions to this list.
        all_suppressed: list = []
        # Aggregate constant-Z folds (BEQ/BNE rewritten to unconditional
        # Goto by the decoder post-pass). Build report at end of pass.
        all_const_z_folds: list = []
        # Aggregate dispatch-target suppressions (decoder rejected an
        # auto-detected dispatch table entry because the target lands
        # inside a cfg `data_region`). Build report.
        all_dispatch_suppressed: list = []
        # Aggregate unresolved indirect JMP/JML/JSR sites. Hard-fail
        # gate: any entry here means a stub would otherwise be emitted.
        all_unresolved_indirects: list = []
        for bank, cfg_path, cfg in parsed:
            out_path = out_dir / f'{args.prefix}_{bank:02x}_v2.c'
            try:
                if cfg.bank != bank:
                    print(f"  {cfg_path.name}: bank field ${cfg.bank:02X} doesn't match filename ${bank:02X}; using filename")
                bank_suppressed: list = []
                bank_const_z_folds: list = []
                bank_dispatch_suppressed: list = []
                bank_unresolved_indirects: list = []
                # Build per-bank `indirect_dispatch` map keyed by
                # 24-bit site PC for the decoder. cfg field is a list
                # of dicts with 16-bit `site_pc16` — combine with this
                # bank to form the 24-bit key.
                ind_dispatch_map = None
                ind_list = getattr(cfg, 'indirect_dispatch', None) or []
                if ind_list:
                    ind_dispatch_map = {}
                    for d in ind_list:
                        pc24 = (bank << 16) | (d['site_pc16'] & 0xFFFF)
                        ind_dispatch_map[pc24] = d
                src = emit_bank(rom, bank=bank, entries=cfg.entries,
                                dispatch_helpers=dispatch_helpers,
                                indirect_call_tables=getattr(
                                    cfg, 'indirect_call_tables', None),
                                indirect_dispatch=ind_dispatch_map,
                                suppressed_collector=bank_suppressed,
                                const_z_fold_collector=bank_const_z_folds,
                                dispatch_target_suppressed_collector=
                                    bank_dispatch_suppressed,
                                unresolved_indirect_collector=
                                    bank_unresolved_indirects,
                                data_regions=cfg.data_regions or None,
                                exclude_ranges=cfg.exclude_ranges or None,
                                callee_exit_mx=callee_exit_mx)
                out_path.write_text(src, encoding='utf-8', newline='\n')
                all_suppressed.extend(bank_suppressed)
                all_const_z_folds.extend(bank_const_z_folds)
                all_dispatch_suppressed.extend(bank_dispatch_suppressed)
                all_unresolved_indirects.extend(bank_unresolved_indirects)
                if pass_idx == 0:
                    print(f"  OK    bank ${bank:02X}: {len(cfg.entries)} entries -> {out_path}")
                succeeded += 1
            except Exception as e:
                print(f"  FAIL  bank ${bank:02X}: {type(e).__name__}: {e}")
                traceback.print_exc()
                failed.append((bank, str(e)))

        # Drain Call-target demands only. Goto targets are no longer
        # auto-promoted (would split asm routines and strand PHB/PLB —
        # the title-screen-loop regression). The decoder imports them
        # into the source function's CFG instead.
        unresolved_calls = take_unresolved_call_targets()
        last_unresolved = unresolved_calls
        if not unresolved_calls:
            break

        added = _autopromote_targets(parsed, unresolved_calls, source_kind="call")

        if added == 0:
            break
        print(
            f"  auto-promote pass {pass_idx + 1}: "
            f"added {added} entries "
            f"(calls={len(unresolved_calls)}); "
            f"re-emitting"
        )

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
            ' * reach them; each stub chains into cpu_trace_unresolved_stub_trap',
            ' * so a runtime fire is captured (loud stderr line + TCP-queryable',
            ' * snapshot via unresolved_stub_get) instead of silently returning.',
            ' * One stub per (target, m, x) variant requested by the gen.',
            ' */',
            '',
            '#include "cpu_state.h"',
            '#include "cpu_trace.h"',
            '',
        ]
        total_stubs = 0
        for bank in sorted(by_bank):
            for pc, em, ex in sorted(by_bank[bank]):
                name = f'bank_{bank:02X}_{pc:04X}_M{em}X{ex}'
                target_pc24 = (bank << 16) | (pc & 0xFFFF)
                lines.append(
                    f'RecompReturn {name}(CpuState *cpu) {{ '
                    f'return cpu_trace_unresolved_stub_trap(cpu, 0x{target_pc24:06x}, "{name}"); '
                    f'}}'
                )
                total_stubs += 1
        stub_path.write_text('\n'.join(lines) + '\n', encoding='utf-8', newline='\n')
        print(f"  emitted stubs for {total_stubs} cross-ROM-bank (target, m, x) variants -> {stub_path}")

    # cfg-required-dispatch-or-kill report. Every JSR (abs,X) site
    # without a cfg `indirect_call_table` directive had its
    # fall-through edge severed at decode. Listed here so the build
    # output is loud rather than silent. Runtime trap (cpu_trace
    # phantom-PC trap) catches any of these PCs that fire.
    if all_suppressed:
        # Collapse to unique (function_entry_pc24, site_pc24, m, x) so
        # repeats across (m,x) variants of the same function show once.
        unique = {}
        for s in all_suppressed:
            key = (s.function_entry_pc24, s.site_pc24, s.entry_m, s.entry_x)
            unique.setdefault(key, s)
        print()
        print(f"=== JSR (abs,X) SUPPRESSED — cfg-required-dispatch-or-kill ===")
        print(f"{len(unique)} unique site/function/(m,x) tuples "
              f"({len(all_suppressed)} total occurrences)")
        # Group by site_pc24 so all (m,x) variants of one site are
        # reported together.
        by_site: dict = {}
        for s in unique.values():
            by_site.setdefault(s.site_pc24, []).append(s)
        for site_pc24 in sorted(by_site.keys()):
            recs = by_site[site_pc24]
            r0 = recs[0]
            mx_set = sorted({(r.entry_m, r.entry_x) for r in recs})
            mx_str = ' '.join(f'M{m}X{x}' for (m, x) in mx_set)
            funcs = sorted({(r.function_entry_pc24) for r in recs})
            funcs_str = ', '.join(
                f'${(f >> 16) & 0xFF:02X}:{f & 0xFFFF:04X}' for f in funcs)
            print(f"  ${site_pc24:06X}  JSR (${r0.table_base:04X},X)  "
                  f"variants[{mx_str}]  in {funcs_str}")
        print(f"Add `indirect_call_table SITE_PC BASE COUNT` to the "
              f"containing function's cfg to authorise.")

    # cfg `data_region` dispatch-target suppressions. Every dispatch
    # table entry the decoder rejected because the target lands inside
    # a declared data_region. Listed so the suppression is visible —
    # never silent — and so the cfg author can audit which entries
    # were dropped per dispatcher.
    if all_dispatch_suppressed:
        # Collapse duplicates (same (site_pc24, target_pc24, reason)
        # may surface from multiple variants of the same dispatcher).
        seen = set()
        unique = []
        for r in all_dispatch_suppressed:
            k = (r.site_pc24, r.target_pc24, r.reason)
            if k in seen:
                continue
            seen.add(k)
            unique.append(r)
        print()
        print("=== DISPATCH TARGET SUPPRESSED BY DATA_REGION ===")
        print(f"{len(unique)} unique (site, target, reason) suppressions "
              f"({len(all_dispatch_suppressed)} total occurrences)")
        for r in sorted(unique, key=lambda x: (x.site_pc24, x.target_pc24)):
            print(f"  bank ${(r.target_pc24 >> 16) & 0xFF:02X}  "
                  f"target ${r.target_pc24 & 0xFFFF:04X}  "
                  f"site ${r.site_pc24:06X}  "
                  f"index {r.table_index}  reason={r.reason}")

    # Constant-Z branch-fold report. Each entry is one BEQ/BNE the
    # decoder rewrote to an unconditional Goto because the same-block
    # predecessor (LDA/LDX/LDY #imm) made Z statically known. The dead
    # edge was pruned along with any insns reachable only through it.
    # Listed here so every fold is visible/auditable rather than silent.
    if all_const_z_folds:
        # Collapse by (branch_pc24, entry_m, entry_x) so the same fold
        # in two (m,x) variants of one function appears twice (each
        # variant has its own decoded body).
        print()
        print(f"=== CONSTANT-Z BRANCH FOLDS ===")
        print(f"{len(all_const_z_folds)} BEQ/BNE rewritten to "
              f"unconditional Goto (decoder post-pass)")
        # Sort by branch PC then by func entry / mode for stable output.
        for f in sorted(all_const_z_folds,
                        key=lambda r: (r.branch_pc24, r.func_entry_pc24,
                                       r.entry_m, r.entry_x)):
            wfmt = f.width_bits // 4   # hex digits
            imm_str = f"#${f.prev_imm:0{wfmt}X}"
            taken_str = 'TAKEN' if f.taken_kind == 'jump' else 'FALL'
            print(f"  ${f.branch_pc24:06X}  "
                  f"{f.prev_mnem} {imm_str} (Z={f.z_value}) ; "
                  f"{f.branch_mnem} -> {taken_str} -> ${f.live_pc24:06X}  "
                  f"[dead -> ${f.dead_pc24:06X}]  "
                  f"in ${f.func_entry_pc24:06X} M{f.entry_m}X{f.entry_x}")

    rejected = take_rejected_call_targets()
    if rejected:
        print()
        print(f"Rejected JSR/JSL targets (out-of-LoROM, decoder followed "
              f"garbage operands) — {len(rejected)} unique addresses:")
        for addr in sorted(rejected):
            print(f"  ${addr:06X}")

    # Unresolved indirect-dispatch sites. Every entry is a JMP/JML/JSR
    # the decoder couldn't resolve via cfg `indirect_dispatch` or auto-
    # recovery. v2_regen treats the list as a hard build error so no
    # IndirectGoto stub silently reaches the runtime. Authoring an
    # `indirect_dispatch <site> <count> idx:<reg> [tables:...]` line in
    # the appropriate bank cfg closes each site; or extend the
    # recompiler with an auto-recovery pattern.
    if all_unresolved_indirects:
        # Group by addressing mode shape for a readable report.
        from collections import defaultdict
        by_form: dict = defaultdict(list)
        for u in all_unresolved_indirects:
            # mode → readable form name (consistent with disassembly).
            form = f"{u.mnem} mode={u.mode}"
            by_form[form].append(u)
        print()
        print(f"=== UNRESOLVED INDIRECT DISPATCH — {len(all_unresolved_indirects)} site(s) ===")
        for form, recs in sorted(by_form.items()):
            print(f"  [{form}] x{len(recs)}")
            for r in sorted(recs, key=lambda x: x.site_pc24):
                print(f"    site=${r.site_pc24:06X}  operand=${r.operand:06X}  "
                      f"M{r.entry_m}X{r.entry_x}  "
                      f"in ${r.function_entry_pc24:06X}")
        print()
        print("Add `indirect_dispatch <site_pc> <count> idx:<X|Y> "
              "[tables:<lo>[,<hi>[,<bank>]]]` to the cfg of each site's "
              "bank to authorise. Stubs are forbidden — see "
              "feedback_no_stubs_ever memory.")

    print()
    print(f"v2_regen: {succeeded}/{total} banks emitted")
    if failed:
        print(f"failed banks:")
        for bank, msg in failed:
            print(f"  ${bank:02X}: {msg}")
        return 1
    # Unresolved IndirectGoto sites: emit runs through (each site
    # produces a runtime cpu_trace_dispatch_oob trap, not a silent
    # stub) but the lint pass below will still flag the build as
    # incomplete via the trap-call string + any other residual
    # stub markers. v2_regen returns non-zero so chained scripts
    # know the recompile didn't close every class.
    if all_unresolved_indirects:
        print(f"  WARN: {len(all_unresolved_indirects)} unresolved "
              f"indirect-dispatch site(s) — trap stubs emitted, HLE pending. "
              f"See report above.")

    # Stub lint. Hard gate: no stub markers in any emitted .c file.
    # See _STUB_MARKERS comment for the rationale. There is no
    # allowlist — every hit is a recompiler-level gap that must be
    # closed at the gen path, not silenced here.
    lint_hits = _lint_stubs(out_dir)
    if lint_hits:
        print()
        print(f"=== STUB LINT — {len(lint_hits)} stub(s) in emitted output ===")
        by_marker: dict[str, list] = {}
        for path, ln, marker, text in lint_hits:
            by_marker.setdefault(marker, []).append((path, ln, text))
        for marker in sorted(by_marker.keys()):
            entries = by_marker[marker]
            print(f"  [{marker}] x{len(entries)}")
            shown = entries[:5]
            for path, ln, text in shown:
                short = pathlib.Path(path).name
                print(f"    {short}:{ln}: {text.strip()[:160]}")
            if len(entries) > len(shown):
                print(f"    ... and {len(entries) - len(shown)} more")
        print()
        print("Stubs are a hard build error. Close the recompiler-level "
              "gap that produced each marker; do NOT add an allowlist.")
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
