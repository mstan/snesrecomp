"""snesrecomp.recompiler.v2.emit_bank

Bank-level emit driver: walks every cfg entry in one bank and emits a
complete C source file containing every function as a separate
`void bank_BB_AAAA(CpuState *cpu)` definition.

Replaces v1's per-bank emit driver (the call-site of `emit_function`
inside `recomp.py`'s top-level `main()`).

Public API:
    emit_bank(rom, bank, entries, *, file_name=None,
              mode_overrides=None) -> str

`entries` is a list of `BankEntry` records describing each function to
emit. The caller (cfg loader, in Phase 6c) is responsible for parsing
the cfg and building this list.
"""

import sys
import pathlib

_THIS_DIR = pathlib.Path(__file__).resolve().parent
_RECOMPILER_DIR = _THIS_DIR.parent
for p in (str(_THIS_DIR), str(_RECOMPILER_DIR)):
    if p not in sys.path:
        sys.path.insert(0, p)

from dataclasses import dataclass  # noqa: E402
from typing import List, Optional, Tuple  # noqa: E402

from v2.emit_function import emit_function  # noqa: E402


@dataclass
class BankEntry:
    """One function to emit in this bank.

    Attributes:
        name: C function name (e.g. "I_RESET", "ProcessNormalSprites").
            If None, defaults to bank_BB_AAAA based on the start PC.
        start: 16-bit local PC, must be in $8000-$FFFF for LoROM.
        end: optional exclusive end PC. If None, decoder runs until
            terminators.
        entry_m, entry_x: entry mode-state. Default (1, 1) — 65816 reset
            state, which is what most SMW functions are entered with.
        tail_call_pc16: optional 16-bit local PC of a SIBLING fn declared
            elsewhere in this bank that this fn deliberately falls
            through into. cfg directive `tail_call:<hex>` on a func line
            sets this. The boundary edge — formerly an unresolvable
            cross-fn goto — gets emitted as
                `return Sibling_M{m}X{x}(cpu);`
            in the calling fn's C body. Encodes a real ROM idiom (two
            asm entry points sharing a body) the recompiler can't
            otherwise prove from bytes alone.
    """
    name: Optional[str]
    start: int
    end: Optional[int] = None
    entry_m: int = 1
    entry_x: int = 1
    tail_call_pc16: Optional[int] = None
    # entry_s_offset: signed adjustment applied to cpu->S when recording
    # _entry_s at function entry.  Use when the function is only ever
    # tail-called from a sibling that left the stack in a non-standard
    # state (e.g. an unmatched PHB), so the host-return check
    # (_hrv && _ret_s == _entry_s) fires correctly.  Default 0 (no
    # adjustment).  Set via cfg `entry_s_offset:<n>` on a func line.
    entry_s_offset: int = 0


def emit_bank(rom: bytes, bank: int,
              entries: List[BankEntry],
              *,
              file_header: Optional[str] = None,
              dispatch_helpers=None,
              indirect_call_tables=None,
              indirect_dispatch=None,
              suppressed_collector=None,
              const_z_fold_collector=None,
              dispatch_target_suppressed_collector=None,
              unresolved_indirect_collector=None,
              data_regions=None,
              exclude_ranges: Optional[List[Tuple[int, int]]] = None,
              callee_exit_mx=None,
              callee_exit_mx_modes=None,
              hle_spc_upload=None,
              hle_func=None,
              hle_dispatch=None,
              inline_arg_map=None) -> str:
    """Emit one bank's C source.

    Args:
        rom: full LoROM image (bytes).
        bank: 8-bit bank number.
        entries: list of BankEntry records — one per function.
        file_header: optional C header lines (includes, banner). If
            None, a default header is emitted.

    Returns:
        Complete C source file as a string. Caller writes it to disk.
    """
    if file_header is None:
        file_header = _default_file_header(bank)

    parts: List[str] = [file_header, ""]

    # Forward decls for every in-bank entry. Each entry emits at the
    # variant-mangled name (`Foo_M{m}X{x}`); calls to later-defined
    # functions need the suffixed declaration to satisfy C lookup.
    parts.append("/* Forward declarations for in-bank entries. */")
    for entry in entries:
        base = entry.name or _default_func_name_local(bank, entry.start)
        suffix = _variant_suffix(entry.entry_m, entry.entry_x)
        parts.append(f"RecompReturn {base}{suffix}(CpuState *cpu);")
    parts.append("")

    # Build a (start_pc16 -> base_name) lookup so a `tail_call:<addr>`
    # directive on one entry can be resolved to the sibling entry's C
    # base name for emission. Mirrors variant_suffix at emit_function
    # call time so the suffix matches the boundary's (m, x).
    by_start: dict = {}
    for e in entries:
        b = e.name or _default_func_name_local(bank, e.start)
        by_start[e.start & 0xFFFF] = b

    # Set of named function entry PCs in THIS bank — passed to the
    # decoder so a cross-end: JUMP that lands on a sibling entry is
    # NOT inline-imported. Without this gate, the decoder pulls the
    # sibling's entire body into the source function's CFG; that's
    # what produced the Zelda intro-loop oscillation (Intro_Init_
    # Continue's BCS to Intro_InitializeMemory_darken inlined darken
    # into Intro_Init_Continue and ran darken's submodule_index++ on
    # the wrong dispatch path). Each entry sees the set minus its own
    # start so back-edges to self stay local.
    all_entry_pcs = {e.start & 0xFFFF for e in entries}

    # Tail-call stack imbalance is handled dynamically by emitted tail
    # transfers: they pass the caller's _entry_s/_hrv to the tail target,
    # and the target prologue consumes that context before recording its
    # frame. Do not auto-mutate entry.entry_s_offset here; a fixed per-entry
    # offset is weaker than the runtime context and conflicts across variants.
    # Manual cfg `entry_s_offset:<n>` remains supported for explicit projects.

    for entry in entries:
        tail_call_target_name = None
        if entry.tail_call_pc16 is not None:
            tgt = entry.tail_call_pc16 & 0xFFFF
            if tgt not in by_start:
                raise ValueError(
                    f"bank ${bank:02X}: func '{entry.name}' at "
                    f"${entry.start:04X} declares tail_call:${tgt:04X} "
                    f"but no sibling func entry exists at that PC. "
                    f"Add the sibling as its own `func` line."
                )
            tail_call_target_name = by_start[tgt]
        sibling_pcs = all_entry_pcs - {entry.start & 0xFFFF}
        if entry.end is not None:
            start16 = entry.start & 0xFFFF
            end16 = entry.end & 0xFFFF

            def _inside_entry_range(pc16: int) -> bool:
                pc16 &= 0xFFFF
                if start16 <= end16:
                    return start16 <= pc16 < end16
                return pc16 >= start16 or pc16 < end16

            # Some asm has multiple callable entry points into one body.
            # If this entry's explicit range includes another entry PC,
            # branches to that PC are local control flow, not sibling
            # tail-calls. Keep the gate for true cross-function jumps.
            sibling_pcs = {
                pc for pc in sibling_pcs
                if not _inside_entry_range(pc)
            }
        src = emit_function(
            rom=rom,
            bank=bank,
            start=entry.start,
            entry_m=entry.entry_m,
            entry_x=entry.entry_x,
            end=entry.end,
            func_name=entry.name,
            dispatch_helpers=dispatch_helpers,
            indirect_call_tables=indirect_call_tables,
            indirect_dispatch=indirect_dispatch,
            suppressed_collector=suppressed_collector,
            const_z_fold_collector=const_z_fold_collector,
            dispatch_target_suppressed_collector=
                dispatch_target_suppressed_collector,
            unresolved_indirect_collector=unresolved_indirect_collector,
            data_regions=data_regions,
            exclude_ranges=exclude_ranges,
            tail_call_pc16=entry.tail_call_pc16,
            tail_call_target_name=tail_call_target_name,
            entry_s_offset=entry.entry_s_offset,
            callee_exit_mx=callee_exit_mx,
            callee_exit_mx_modes=callee_exit_mx_modes,
            sibling_entry_pcs=sibling_pcs,
            hle_spc_upload=hle_spc_upload,
            hle_func=hle_func,
            hle_dispatch=hle_dispatch,
            inline_arg_map=inline_arg_map,
        )
        parts.append(src)
        parts.append("")  # blank line between functions

    # Aliases for cfg-named entries — un-suffixed wrapper that calls
    # into one specific variant. Hand-written entry-point shims (e.g.
    # smw_rtl.c calling `I_RESET(&g_cpu)`) bind to these. The alias
    # picks the cfg-declared (entry_m, entry_x) — i.e. the canonical
    # entry for that name. If a function has multiple (m,x) variants
    # only one alias is emitted (the cfg-default); other variants are
    # reachable only through gen-emitted Call ops that mangle names.
    # Aliases stay `void` for ABI compatibility with hand-written
    # callers (smw_rtl.c calls `I_RESET(&g_cpu)` etc.). They abort
    # loudly if a non-NORMAL RecompReturn propagates up to the v2
    # boundary — that would mean a SKIP_N idiom leaked past the v2
    # region into hand-written code, which is a design violation
    # worth crashing on.
    aliased: set = set()
    for entry in entries:
        if not entry.name:
            continue
        if entry.name in aliased:
            continue
        suffix = _variant_suffix(entry.entry_m, entry.entry_x)
        aliased.add(entry.name)
        parts.append(
            f"void {entry.name}(CpuState *cpu) {{\n"
            f"  RecompReturn _r = {entry.name}{suffix}(cpu);\n"
            f"  if (_r != RECOMP_RETURN_NORMAL) {{\n"
            f"    fprintf(stderr,\n"
            f"      \"[recomp] non-local-return SKIP_%d leaked past void alias %s\\n\",\n"
            f"      (int)_r, \"{entry.name}\");\n"
            f"    abort();\n"
            f"  }}\n"
            f"}}"
        )
    if aliased:
        parts.append("")

    return "\n".join(parts)


def _default_func_name_local(bank: int, start: int) -> str:
    return f"bank_{bank:02X}_{start:04X}"


def _variant_suffix(m: int, x: int) -> str:
    """Mirror of codegen._variant_suffix — duplicated to avoid the
    cross-module import cycle. Must stay in sync."""
    return f"_M{m & 1}X{x & 1}"


def _default_file_header(bank: int) -> str:
    return f"""\
/* Auto-generated by snesrecomp v2 emit_bank. Do NOT hand-edit.
 *
 * Bank ${bank:02X}. Each function below mutates the shared CpuState
 * struct via cpu->A / cpu->X / etc. Memory access goes through the
 * cpu_read{{8,16}} / cpu_write{{8,16}} helpers in cpu_state.h.
 */

#include <stdio.h>
#include <stdlib.h>

#include "cpu_state.h"
#include "cpu_trace.h"
#include "common_cpu_infra.h"
#include "funcs.h"
"""
