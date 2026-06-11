"""snesrecomp.recompiler.v2.cfg_loader

Parse a v1-format bank cfg file into a v2 `BankCfg` dataclass that
`emit_bank` can consume.

v2 IGNORES the v1 ABI-fiction directives:
- `sig:` — every v2 function is `void f(CpuState *cpu)`.
- `ret_y` / `carry_ret` — return value is never materialised; flags
  and registers live in the CpuState struct.
- `y_after:` / `x_after:` — call-site index increments are dead with
  the new ABI (caller and callee both see cpu->X / cpu->Y).
- `restores_x:` / DP-aliased struct params (e.g. `CollInfo_*ci`) —
  same, dead.

v2 KEEPS:
- `bank = NN`
- `includes = ...` (used to extend the default emit_bank header).
- `func <name> <hex_pc> [end:<hex_end>] [entry_mx:M,X]` — the entry
  list. `entry_mx` overrides the default reset-state M/X width for
  hand-written entries whose ABI is only valid after caller-side setup.
- `entry_mx_at <hex_pc> <M> <X>` — the same entry-width override, kept
  outside auto-ingested func blocks so those blocks stay regeneratable.
- `end_at <hex_pc> <hex_end>` — override a generated function's
  exclusive end boundary outside auto-ingested func blocks.
- `name <hex_addr> <name>` — friendly-naming for cross-bank labels.
  v2 emits these as `void NAME(CpuState *cpu);` forward declarations
  in funcs.h (Phase 6e/f). For now we just retain them.
- `exclude_range <start> <end>` — data region carved out of decode.
- `data_region <bank> <start> <end>` — same idea, cross-bank.

Public API:
    load_bank_cfg(path: str) -> BankCfg
"""

import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Optional, Tuple

import sys

_THIS_DIR = Path(__file__).resolve().parent
_RECOMPILER_DIR = _THIS_DIR.parent
for p in (str(_THIS_DIR), str(_RECOMPILER_DIR)):
    if p not in sys.path:
        sys.path.insert(0, p)

from v2.emit_bank import BankEntry  # noqa: E402


@dataclass
class NameDecl:
    """A `name <addr> <friendly>` line. Cross-bank label / friendly
    name used by funcs.h forward decls; not an entry to emit here."""
    addr_24: int  # bank << 16 | local pc
    name: str


@dataclass
class BankCfg:
    bank: int
    includes: List[str] = field(default_factory=list)
    entries: List[BankEntry] = field(default_factory=list)
    names: List[NameDecl] = field(default_factory=list)
    exclude_ranges: List[Tuple[int, int]] = field(default_factory=list)
    data_regions: List[Tuple[int, int, int]] = field(default_factory=list)  # (bank, start, end)
    # exit_mx_at directives: list of (bank, addr16, m, x) — annotates the
    # exit (m, x) state of a function at that PC. Decoder uses this to
    # resume callers after JSR/JSL with the correct (m, x) instead of
    # assuming the callee preserves the caller's state. THIS LIST IS FOR
    # HAND-WRITTEN cfg DIRECTIVES ONLY — the auto-router populates
    # `exit_mx_at_per_variant` (below) which is per-entry-variant and
    # always correct; the 4-tuple broadcast here is preserved for
    # backward compat with hand-written hints that pre-date the
    # per-variant work.
    exit_mx_at: List[Tuple[int, int, int, int]] = field(default_factory=list)
    # Per-entry-variant exit (m, x) tuples populated by the auto-router.
    # Each entry is (bank, addr16, entry_m, entry_x, exit_m, exit_x).
    # Consumed by v2_regen.py's callee_exit_mx builder; per-variant
    # entries OVERRIDE the legacy 4-tuple broadcast for their specific
    # (target, entry_m, entry_x) keys. This is the proper data model
    # for functions whose exit depends on entry — the broadcast 4-tuple
    # is wrong for those (Bug C class, see
    # docs/ABSTRACT_INTERPRETATION_GAPS.md).
    exit_mx_at_per_variant: List[Tuple[int, int, int, int, int, int]] = field(default_factory=list)
    # `auto_vectors` directive — when true and this cfg is bank 00,
    # v2_regen reads the SNES interrupt-vector table at ROM offset
    # 0x7FE0-0x7FFF (LoROM mirror of $00:FFE0-FFFF) and auto-seeds
    # `func I_RESET / I_NMI / I_IRQ` entries at the dereferenced PCs.
    # Lets a fresh game project ship a minimal bank00.cfg with just
    # the one directive instead of hand-decoding the vector table.
    auto_vectors: bool = False
    # `indirect_dispatch` directives — authorise the decoder to recover
    # the static target list of an indirect JMP/JML/JSR. Each entry is
    # a dict with keys:
    #   site_pc16: int       — local 16-bit PC of the dispatch insn
    #   count: int           — N (number of dispatch targets)
    #   idx_reg: 'X' or 'Y'  — which CPU index register selects the case
    #   table_bases: tuple   — table addr(s):
    #       ()           : table base taken from insn.operand (JSR/JMP/JML (abs,X))
    #       (lo,)        : single static table at given addr
    #       (lo, hi)     : two parallel byte-tables forming a 16-bit pointer
    #       (lo, hi, bk) : three parallel byte-tables forming a 24-bit pointer
    #                      (assembled into DP then JML [DP], e.g. Module_MainRouting)
    # Decoder reads ROM via the resolved table layout, registers each
    # entry as a decode successor (for auto-promote / reachability), and
    # stamps `insn.dispatch_entries` so codegen emits a real switch.
    indirect_dispatch: List[dict] = field(default_factory=list)
    # `hle_spc_upload <pc>` directives — replace the recompiled body of
    # the function starting at <pc> with a single call to the runtime
    # HLE helper RtlUploadSpcImageFromDp. The standard SNES SPC upload
    # protocol is a length/target/data block stream pointed at by a
    # 24-bit ROM pointer in direct page ($DP+0..2). The runtime reads
    # the stream directly into apu->ram and jumps apu->spc->pc to the
    # terminator's target, bypassing the per-byte IPL handshake (which
    # only really works under hardware-realistic SPC pacing and pins
    # the recompiler against the watchdog for many wall seconds).
    # Per-game: declare the SPC upload entry PC here. Works for SMW
    # (HandleSPCUploads_Inner / SPC700UploadLoop at $00:8079) and
    # ALttP (LoadSongBank at $00:8888) — both use the same protocol.
    hle_spc_upload: List[int] = field(default_factory=list)
    # `hle_func <pc16> <c_function_name>` — replace the recompiled body
    # of the function at <pc> with a single forwarding call to the named
    # C function. Used for hand-written HLE bodies that need to run
    # alongside cfg-declared (m,x) variants — the recompiler skips
    # decoding the bytes for these PCs and emits a forwarding stub per
    # requested variant: `RecompReturn NAME_MxXy(CpuState *cpu) {
    #   return <c_function_name>(cpu); }`. The C helper must be
    # provided by the per-game runner (typically in gen_stubs.c).
    # Map: pc16 -> c_function_name. See emit_function.py for stub shape.
    hle_func: dict = field(default_factory=dict)
    # `hle_dispatch <site_pc16> <c_function_name>` — at the named indirect
    # JMP/JML/JSR site, replace the unresolved-dispatch trap with a
    # tail-call to the named C helper. Used when the asm dispatcher at
    # <site_pc16> is replaced by a host-side scheduler / interpreter
    # that decides the next PC itself (e.g. the MMX cooperative-task
    # scheduler's `JMP ($0032,X)` at $00:80E6 — the C-host MmxSchedulerTick
    # selects the task body to run). The site PC matches insn.addr,
    # so the helper fires from every caller-body that inlined the
    # dispatch as part of its decoded CFG. Map: pc16 -> c_function_name.
    hle_dispatch: dict = field(default_factory=dict)
    # `force_variant_at <site_pc24> <m> <x>` — at the named direct
    # JSR/JSL site, bypass the runtime 4-way (m, x) dispatch and emit a
    # hardcoded call to the (m, x) variant of the target. Used for
    # diagnostic VALIDATION of suspected m-flag tracking bugs: if forcing
    # the expected variant at a specific site makes a freeze go away,
    # the runtime cpu->m_flag at that site is wrong and the root cause
    # is upstream. Once the upstream bug is fixed the hint should be
    # REMOVED — this directive is intentionally narrow (one site, one
    # (m, x)) and is NOT a long-term workaround. The 24-bit PC matches
    # the JSR/JSL instruction's own address (the `insn.addr` of the
    # call), NOT the target. Map: site_pc24 -> (m, x).
    force_variant_at: dict = field(default_factory=dict)


# Token regex helpers
_HEX_RE = re.compile(r'^[0-9a-fA-F]+$')


def _parse_hex(token: str) -> int:
    if token.lower().startswith('0x'):
        return int(token, 16)
    return int(token, 16)


def _strip_comment(line: str) -> str:
    """Strip trailing '# ...' comment from a cfg line."""
    idx = line.find('#')
    if idx >= 0:
        line = line[:idx]
    return line.rstrip()


def load_bank_cfg(path: str) -> BankCfg:
    """Parse a v1-format bank cfg file. Returns a BankCfg dataclass.

    Raises ValueError on a malformed `bank =` line. Other unrecognized
    directives are silently ignored (forward-compat with v1 cfgs that
    have v1-only directives the v2 pipeline doesn't need).
    """
    cfg = BankCfg(bank=-1)
    entry_mx_at: dict[int, Tuple[int, int]] = {}
    end_at: dict[int, int] = {}

    with open(path, 'r', encoding='utf-8', errors='replace') as fp:
        for raw in fp:
            line = raw.rstrip('\n')

            # Strip comments + leading whitespace for everything else.
            stripped = _strip_comment(line).strip()
            if not stripped:
                continue

            tokens = stripped.split()
            head = tokens[0]

            # bank = NN
            if head == 'bank' and len(tokens) >= 3 and tokens[1] == '=':
                cfg.bank = _parse_hex(tokens[2])
                continue

            # includes = a.h b.h c.h
            if head == 'includes' and len(tokens) >= 3 and tokens[1] == '=':
                cfg.includes = list(tokens[2:])
                continue

            # comment = ... (v2 ignores)
            if head == 'comment' and '=' in stripped:
                continue

            # auto_vectors — request that v2_regen.py read the SNES
            # interrupt-vector table at ROM offset 0x7FE0-0x7FFF
            # (LoROM mirror of $00:FFE0-FFFF) and auto-seed
            # `func I_RESET / I_NMI / I_IRQ` entries. Bank 00 only
            # (vectors live in bank 0); v2_regen warns and skips for
            # other banks.
            if head == 'auto_vectors':
                cfg.auto_vectors = True
                continue

            # entry_mx_at <pc16> <m> <x> — override a cfg entry's
            # canonical decode width without modifying auto-ingested
            # `func` lines.
            if head == 'entry_mx_at':
                if len(tokens) != 4:
                    raise ValueError(
                        f"{path}: entry_mx_at needs <pc16> <m> <x>, "
                        f"got: {stripped!r}")
                try:
                    pc16 = _parse_hex(tokens[1]) & 0xFFFF
                    m_val = int(tokens[2]) & 1
                    x_val = int(tokens[3]) & 1
                except ValueError as e:
                    raise ValueError(
                        f"{path}: entry_mx_at bad argument in "
                        f"{stripped!r}: {e}")
                entry_mx_at[pc16] = (m_val, x_val)
                continue

            # end_at <pc16> <end> — override a cfg entry's exclusive
            # decode boundary without modifying auto-ingested `func`
            # lines.
            if head == 'end_at':
                if len(tokens) != 3:
                    raise ValueError(
                        f"{path}: end_at needs <pc16> <end>, "
                        f"got: {stripped!r}")
                try:
                    pc16 = _parse_hex(tokens[1]) & 0xFFFF
                    end_val = _parse_hex(tokens[2])
                except ValueError as e:
                    raise ValueError(
                        f"{path}: end_at bad argument in "
                        f"{stripped!r}: {e}")
                end_at[pc16] = end_val
                continue

            # hle_spc_upload <hex_pc> — mark the function at <pc> as
            # the project's SPC upload entry. emit_function replaces
            # its decoded body with a single RtlUploadSpcImageFromDp
            # call; the runtime walks the standard length/target/data
            # block stream pointed to by DP+0..2 and writes directly
            # into apu->ram. See BankCfg.hle_spc_upload comment.
            if head == 'hle_spc_upload':
                if len(tokens) != 2:
                    raise ValueError(
                        f"{path}: hle_spc_upload needs exactly one <pc> "
                        f"argument, got: {stripped!r}")
                try:
                    pc16 = _parse_hex(tokens[1]) & 0xFFFF
                except ValueError as e:
                    raise ValueError(
                        f"{path}: hle_spc_upload bad pc {tokens[1]!r}: {e}")
                cfg.hle_spc_upload.append(pc16)
                continue

            # hle_func <pc16> <c_function_name> — replace the decoded
            # body of the function at <pc> with a forwarding stub:
            #   RecompReturn NAME_MxXy(CpuState *cpu) {
            #     return <c_function_name>(cpu);
            #   }
            # The C helper must be provided externally (typically
            # gen_stubs.c). One stub per (m,x) variant the recompiler
            # discovers as called from elsewhere. See BankCfg.hle_func
            # comment.
            if head == 'hle_func':
                if len(tokens) != 3:
                    raise ValueError(
                        f"{path}: hle_func needs <pc> <c_function_name>, "
                        f"got: {stripped!r}")
                try:
                    pc16 = _parse_hex(tokens[1]) & 0xFFFF
                except ValueError as e:
                    raise ValueError(
                        f"{path}: hle_func bad pc {tokens[1]!r}: {e}")
                c_name = tokens[2]
                if not c_name.replace('_', '').isalnum() or c_name[0].isdigit():
                    raise ValueError(
                        f"{path}: hle_func c_function_name must be a valid "
                        f"C identifier, got: {c_name!r}")
                cfg.hle_func[pc16] = c_name
                continue

            # force_variant_at <site_pc24> <m> <x> — pin the variant
            # called at the named direct JSR/JSL site. See BankCfg
            # field doc for use as a diagnostic for suspected m-flag
            # tracking bugs. Format: site_pc24 hex (with or without 0x),
            # m and x each 0 or 1.
            if head == 'force_variant_at':
                if len(tokens) != 4:
                    raise ValueError(
                        f"{path}: force_variant_at needs <site_pc24> <m> <x>, "
                        f"got: {stripped!r}")
                try:
                    site_pc24 = _parse_hex(tokens[1]) & 0xFFFFFF
                except ValueError as e:
                    raise ValueError(
                        f"{path}: force_variant_at bad site_pc24 {tokens[1]!r}: {e}")
                try:
                    m_val = int(tokens[2])
                    x_val = int(tokens[3])
                except ValueError as e:
                    raise ValueError(
                        f"{path}: force_variant_at m and x must be 0 or 1: {e}")
                if m_val not in (0, 1) or x_val not in (0, 1):
                    raise ValueError(
                        f"{path}: force_variant_at m and x must be 0 or 1, "
                        f"got m={m_val} x={x_val}")
                if site_pc24 in cfg.force_variant_at:
                    raise ValueError(
                        f"{path}: force_variant_at duplicate site ${site_pc24:06X}")
                cfg.force_variant_at[site_pc24] = (m_val, x_val)
                continue

            # hle_dispatch <site_pc16> <c_function_name> — replace the
            # unresolved-dispatch trap at the indirect JMP/JML/JSR site
            # at <site_pc16> with a tail-call to the named C helper.
            # The helper decides the next PC (host scheduler / interpreter).
            # See BankCfg.hle_dispatch.
            if head == 'hle_dispatch':
                if len(tokens) != 3:
                    raise ValueError(
                        f"{path}: hle_dispatch needs <site_pc16> <c_function_name>, "
                        f"got: {stripped!r}")
                try:
                    pc16 = _parse_hex(tokens[1]) & 0xFFFF
                except ValueError as e:
                    raise ValueError(
                        f"{path}: hle_dispatch bad pc {tokens[1]!r}: {e}")
                c_name = tokens[2]
                if not c_name.replace('_', '').isalnum() or c_name[0].isdigit():
                    raise ValueError(
                        f"{path}: hle_dispatch c_function_name must be a valid "
                        f"C identifier, got: {c_name!r}")
                cfg.hle_dispatch[pc16] = c_name
                continue

            # indirect_dispatch <site_pc> <count> idx:<X|Y> [tables:<lo>[,<hi>[,<bank>]]]
            #
            # Authorises the decoder to recover the static target list
            # of an indirect JMP/JML/JSR at the named site. See
            # BankCfg.indirect_dispatch for the field shape + table-base
            # interpretation. Class fix for IndirectGoto / Call indirect
            # SUPPRESSED stubs — see _STUB_MARKERS in tools/v2_regen.py
            # and feedback_no_stubs_ever memory.
            if head == 'indirect_dispatch':
                if len(tokens) < 4:
                    raise ValueError(
                        f"{path}: indirect_dispatch needs at least "
                        f"<site_pc> <count> idx:<reg> — got: {stripped!r}")
                try:
                    site_pc16 = _parse_hex(tokens[1]) & 0xFFFF
                except ValueError as e:
                    raise ValueError(
                        f"{path}: indirect_dispatch bad site_pc {tokens[1]!r}: {e}")
                try:
                    count = int(tokens[2], 0)
                except ValueError as e:
                    raise ValueError(
                        f"{path}: indirect_dispatch bad count {tokens[2]!r}: {e}")
                if count <= 0 or count > 4096:
                    raise ValueError(
                        f"{path}: indirect_dispatch count {count} out of range (1..4096)")
                # Pointer-sourced CALL form (the `PEA <ret>; JMP (ptr)` /
                # `JSR (ptr)` idiom, where `ptr` is a WRAM word holding the
                # current handler address — e.g. SM's cinematic_function).
                # Unlike the register-indexed table form, there is no ROM
                # table to walk: the candidate target set is given explicitly
                # (sourced from the decomp's dispatcher switch). The dispatch
                # is a CALL (the pushed PEA return resumes the next block),
                # not a tail transfer.
                #   indirect_dispatch <site_pc16> <count> ptrcall targets:<t1,t2,...>
                #
                # Targets may be 16-bit local PCs or full 24-bit SNES
                # pointers. Keep full-width values intact: long pointer
                # dispatches such as SM's HDMA pre-instruction call need
                # to distinguish $88:EBB0 from local $08:EBB0.
                if 'ptrcall' in tokens[3:]:
                    targets: Tuple[int, ...] = ()
                    for t in tokens[3:]:
                        if t == 'ptrcall':
                            continue
                        if t.startswith('targets:'):
                            raw = t[len('targets:'):].split(',')
                            try:
                                targets = tuple(_parse_hex(b) & 0xFFFFFF for b in raw if b)
                            except ValueError as e:
                                raise ValueError(
                                    f"{path}: indirect_dispatch targets: bad hex {t!r}: {e}")
                        else:
                            raise ValueError(
                                f"{path}: indirect_dispatch ptrcall unknown option {t!r}")
                    if not targets:
                        raise ValueError(
                            f"{path}: indirect_dispatch ptrcall needs targets:<...> — got: {stripped!r}")
                    if len(targets) != count:
                        raise ValueError(
                            f"{path}: indirect_dispatch ptrcall count {count} != "
                            f"{len(targets)} targets")
                    cfg.indirect_dispatch.append({
                        'site_pc16': site_pc16,
                        'count': count,
                        'idx_reg': 'X',          # unused (value-matched), kept for emit path
                        'table_bases': (),
                        'ptr_call': True,
                        'targets': targets,
                    })
                    continue

                idx_reg: Optional[str] = None
                table_bases: Tuple[int, ...] = ()
                for t in tokens[3:]:
                    if t.startswith('idx:'):
                        v = t[len('idx:'):].upper()
                        if v not in ('X', 'Y'):
                            raise ValueError(
                                f"{path}: indirect_dispatch idx: must be X or Y, got {v!r}")
                        idx_reg = v
                    elif t.startswith('tables:'):
                        raw_bases = t[len('tables:'):].split(',')
                        if len(raw_bases) < 1 or len(raw_bases) > 3:
                            raise ValueError(
                                f"{path}: indirect_dispatch tables: needs 1-3 "
                                f"comma-separated hex addresses, got {t!r}")
                        try:
                            table_bases = tuple(_parse_hex(b) & 0xFFFF for b in raw_bases)
                        except ValueError as e:
                            raise ValueError(
                                f"{path}: indirect_dispatch tables: bad hex {t!r}: {e}")
                    else:
                        raise ValueError(
                            f"{path}: indirect_dispatch unknown option {t!r}")
                if idx_reg is None:
                    raise ValueError(
                        f"{path}: indirect_dispatch needs idx:X or idx:Y — got: {stripped!r}")
                cfg.indirect_dispatch.append({
                    'site_pc16': site_pc16,
                    'count': count,
                    'idx_reg': idx_reg,
                    'table_bases': table_bases,
                })
                continue

            # func <name> <hex_pc> [end:<hex_end>] [entry_mx:M,X] [tail_call:<hex>] [exit_mx:M,X]
            #                      [entry_s_offset:<n>] [sig:...] [...]
            if head == 'func':
                if len(tokens) < 3:
                    continue
                name = tokens[1]
                start = _parse_hex(tokens[2])
                end: Optional[int] = None
                entry_m, entry_x = entry_mx_at.get(start & 0xFFFF, (1, 1))
                tail_call_pc16: Optional[int] = None
                exit_mx: Optional[Tuple[int, int]] = None
                entry_s_offset_val: int = 0
                for t in tokens[3:]:
                    if t.startswith('end:'):
                        try:
                            end = _parse_hex(t[len('end:'):])
                        except ValueError:
                            pass
                    elif t.startswith('tail_call:'):
                        # Local 16-bit PC of a sibling fn whose body
                        # this one falls into. The sibling MUST be
                        # declared as its own `func` entry elsewhere in
                        # this same bank cfg; emit_bank validates that
                        # at resolve time.
                        try:
                            tail_call_pc16 = _parse_hex(t[len('tail_call:'):])
                        except ValueError:
                            pass
                    elif t.startswith('exit_mx:'):
                        # Per-function callee-exit (m, x) annotation.
                        # Format: exit_mx:M,X with M and X each 0 or 1.
                        # When the decoder hits a JSR/JSL whose target's
                        # cfg entry has this directive, it resumes the
                        # caller at the annotated (m, x) instead of
                        # assuming the callee preserves the caller's
                        # state. Required for callees that internally
                        # SEP/REP without restoring (e.g. SMW $00:F465
                        # sets m=1 via SEP #$20 at entry, never resets,
                        # so callers in m=0 must resume at m=1 — without
                        # the annotation, decoder mis-decodes operand
                        # widths and synthesises phantom branch targets
                        # at mid-instruction bytes; root cause of the
                        # RunPlayerBlockCode -1 stack drift / Mario-
                        # dies-on-slope bug, 2026-05-03).
                        try:
                            mx_str = t[len('exit_mx:'):]
                            parts = mx_str.split(',')
                            if len(parts) == 2:
                                exit_mx = (int(parts[0]) & 1,
                                           int(parts[1]) & 1)
                        except (ValueError, IndexError):
                            pass
                    elif t.startswith('entry_mx:'):
                        try:
                            mx_str = t[len('entry_mx:'):]
                            parts = mx_str.split(',')
                            if len(parts) == 2:
                                entry_m = int(parts[0]) & 1
                                entry_x = int(parts[1]) & 1
                        except (ValueError, IndexError):
                            pass
                    elif t.startswith('entry_s_offset:'):
                        try:
                            entry_s_offset_val = int(t[len('entry_s_offset:'):])
                        except ValueError:
                            pass
                if (start & 0xFFFF) in end_at:
                    end = end_at[start & 0xFFFF]
                be = BankEntry(
                    name=name, start=start, end=end,
                    entry_m=entry_m, entry_x=entry_x,
                    tail_call_pc16=tail_call_pc16,
                    entry_s_offset=entry_s_offset_val)
                # Non-default attribute on BankEntry; assign post-init.
                if exit_mx is not None:
                    be.exit_mx = exit_mx
                cfg.entries.append(be)
                continue

            # name <hex_addr> <friendly_name> [sig:...]
            if head == 'name':
                if len(tokens) < 3:
                    continue
                try:
                    addr = _parse_hex(tokens[1])
                except ValueError:
                    continue
                friendly = tokens[2]
                cfg.names.append(NameDecl(addr_24=addr, name=friendly))
                continue

            # exclude_range <start> <end>
            if head == 'exclude_range' and len(tokens) >= 3:
                try:
                    s = _parse_hex(tokens[1])
                    e = _parse_hex(tokens[2])
                except ValueError:
                    continue
                cfg.exclude_ranges.append((s, e))
                continue

            # exit_mx_at <hex_24bit_addr> <m> <x>
            #
            # Stand-alone callee-exit-(m,x) annotation. Records that the
            # function entry at the given 24-bit address returns with
            # the named (m, x). Independent of `func` entries — useful
            # when the function is a callee discovered via auto-promote
            # (e.g. SMW $00:F461 is reached only via JSR from inside
            # other functions; it has no cfg `func` line, so the
            # annotation goes here). Bank is encoded in the high byte
            # of the address; format `<bank><addr16>` as 6 hex digits.
            if head == 'exit_mx_at' and len(tokens) >= 4:
                try:
                    addr_24 = _parse_hex(tokens[1])
                    m_val = int(tokens[2]) & 1
                    x_val = int(tokens[3]) & 1
                except ValueError:
                    continue
                bank_id = (addr_24 >> 16) & 0xFF
                addr16 = addr_24 & 0xFFFF
                cfg.exit_mx_at.append((bank_id, addr16, m_val, x_val))
                continue

            # data_region <bank> <start> <end>
            if head == 'data_region' and len(tokens) >= 4:
                try:
                    b = _parse_hex(tokens[1])
                    s = _parse_hex(tokens[2])
                    e = _parse_hex(tokens[3])
                except ValueError:
                    continue
                cfg.data_regions.append((b, s, e))
                continue

            # Anything else: silently ignore (v1-only directive or
            # forward-compat).

    if cfg.bank < 0:
        raise ValueError(f"{path}: missing 'bank = NN' line")

    for entry in cfg.entries:
        override = entry_mx_at.get(entry.start & 0xFFFF)
        if override is not None:
            entry.entry_m, entry.entry_x = override
        end_override = end_at.get(entry.start & 0xFFFF)
        if end_override is not None:
            entry.end = end_override

    # Auto-promote in-bank `name <addr> <friendly>` declarations to emit
    # entries. v1's recompiler auto-promoted JSL/JSR targets into their
    # own functions; v2 doesn't, so JSR/JSL targets named in cfg but
    # without an explicit `func` entry would otherwise be referenced
    # without a definition. (Cross-bank `name` lines stay declaration-
    # only — their owning bank's cfg is responsible for emitting them.)
    existing_starts = {e.start & 0xFFFF for e in cfg.entries}
    for nd in cfg.names:
        if (nd.addr_24 >> 16) & 0xFF != cfg.bank:
            continue
        local_pc = nd.addr_24 & 0xFFFF
        if local_pc in existing_starts:
            continue
        cfg.entries.append(BankEntry(name=nd.name, start=local_pc))
        existing_starts.add(local_pc)

    return cfg
