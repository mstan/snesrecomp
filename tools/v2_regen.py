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
    3. write to <out_dir>/bankXX_v2.c  (game-agnostic naming)

Exits 0 if every bank completed; non-zero otherwise. Per-bank failures
are caught and reported individually so a single bug doesn't block
the rest of the integration run.
"""

import argparse
import atexit
import os
import pathlib
import re
import shutil
import sys
import tempfile
import threading
import time
import traceback

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / 'recompiler'))

from snes65816 import load_rom  # noqa: E402
from v2.cfg_loader import load_bank_cfg  # noqa: E402
from v2.codegen import (  # noqa: E402
    set_name_resolver,
    set_rom_size,
    set_force_variant_at,
    set_valid_variants,
    set_trampoline_returns,
    take_rejected_call_targets,
    take_trampoline_returns,
    take_unresolved_call_targets,
    take_unresolved_goto_targets,
)
from v2.decoder import (  # noqa: E402
    classify_dispatch_helper, decode_function, analyze_function_exit_mx,
    detect_inline_arg_bytes, set_active_inline_arg_map,
    set_decode_cache_enabled, clear_decode_cache, decode_cache_stats,
)
from v2.emit_bank import BankEntry, emit_bank  # noqa: E402
from v2.wrapper_autoroute import detect_and_route as autoroute_wrappers, format_fix_summary  # noqa: E402
from v2.tail_call_autoroute import (  # noqa: E402
    detect_and_route as autoroute_tail_calls,
    format_fix_summary as format_tail_call_summary,
)
from v2.exit_mx_autoroute import (  # noqa: E402
    detect_and_route as autoroute_exit_mx,
    format_fix_summary as format_exit_mx_summary,
)
from v2.pha_rts_autoroute import (  # noqa: E402
    detect_and_route as autoroute_pha_rts,
    format_fix_summary as format_pha_rts_summary,
)


_BANK_CFG_RE = re.compile(r'bank([0-9a-fA-F]+)\.cfg$')


class _AtomicOutputDir:
    """Transactional generated-output directory on the target filesystem.

    Existing files are hard-linked into a sibling staging directory (copy2 is
    the fallback). ``write_if_changed`` replaces changed staging files, so a
    hard link is never modified in place. The live output remains untouched
    throughout analysis and convergence.

    Publish swaps whole directories. A deterministic previous-directory name
    makes an interruption recoverable on the next invocation.
    """

    def __init__(self, target: pathlib.Path):
        self.target = target.resolve()
        self.parent = self.target.parent
        self.parent.mkdir(parents=True, exist_ok=True)
        self.previous = self.parent / f'.{self.target.name}.snesrecomp-previous'
        self.staging: pathlib.Path | None = None
        self.published = False

        self._recover_interrupted_publish()
        raw = tempfile.mkdtemp(
            prefix=f'.{self.target.name}.snesrecomp-staging-',
            dir=str(self.parent))
        self.staging = pathlib.Path(raw)
        if self.target.exists():
            self._link_existing_tree(self.target, self.staging)
        atexit.register(self.cleanup)

    def _recover_interrupted_publish(self) -> None:
        if self.previous.exists() and not self.target.exists():
            os.replace(self.previous, self.target)
        elif self.previous.exists() and self.target.exists():
            shutil.rmtree(self.previous)

    @staticmethod
    def _link_existing_tree(source: pathlib.Path,
                            destination: pathlib.Path) -> None:
        for root, dirnames, filenames in os.walk(source):
            root_path = pathlib.Path(root)
            relative = root_path.relative_to(source)
            dest_root = destination / relative
            dest_root.mkdir(parents=True, exist_ok=True)
            for dirname in dirnames:
                (dest_root / dirname).mkdir(exist_ok=True)
            for filename in filenames:
                src = root_path / filename
                dst = dest_root / filename
                try:
                    os.link(src, dst)
                except OSError:
                    shutil.copy2(src, dst)

    def publish(self) -> None:
        if self.staging is None or self.published:
            raise RuntimeError('generated-output workspace is not publishable')
        if self.previous.exists():
            shutil.rmtree(self.previous)
        moved_live = False
        try:
            if self.target.exists():
                os.replace(self.target, self.previous)
                moved_live = True
            os.replace(self.staging, self.target)
            self.staging = None
            self.published = True
        except BaseException:
            if moved_live and not self.target.exists() and self.previous.exists():
                os.replace(self.previous, self.target)
            raise
        if self.previous.exists():
            shutil.rmtree(self.previous)

    def cleanup(self) -> None:
        if self.staging is not None and self.staging.exists():
            shutil.rmtree(self.staging, ignore_errors=True)
        self.staging = None


def write_if_changed(path, content: str) -> bool:
    """Write `content` to `path` only if it differs from what's already
    on disk. Returns True if a write happened, False if skipped.

    Optimization (2026-05-31): the regen rewrites every generated .c on
    every fixpoint pass. The vast majority of banks are byte-identical
    pass-to-pass (and run-to-run), so unconditional writes needlessly
    bump mtimes -> the C build then recompiles every bank object even
    when only one bank's bytes actually changed. Content-gating the
    write preserves mtimes on unchanged files so the incremental build
    skips their objects. Newline normalized to '\\n' to match the
    write_text(..., newline='\\n') the callers used.
    """
    try:
        existing = path.read_text(encoding='utf-8')
        # read_text() translates platform newlines; compare against the
        # same normalization the writer applies.
        if existing == content:
            return False
    except (FileNotFoundError, OSError, UnicodeDecodeError):
        pass
    path.parent.mkdir(parents=True, exist_ok=True)
    # Staging initially hard-links unchanged live files. Replace changed files
    # rather than truncating them, which would mutate the live inode too.
    fd, tmp_name = tempfile.mkstemp(
        prefix=f'.{path.name}.tmp-', dir=str(path.parent), text=True)
    try:
        with os.fdopen(fd, 'w', encoding='utf-8', newline='\n') as f:
            f.write(content)
        fd = -1
        os.replace(tmp_name, path)
    except BaseException:
        if fd >= 0:
            try:
                os.close(fd)
            except OSError:
                pass
        try:
            os.unlink(tmp_name)
        except OSError:
            pass
        raise
    return True


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
    'Goto with no successor',                # emit_function cross-bank-goto bail (2026-05-18)
    'unresolvable cross-bank goto',          # emit_function cross-bank trap fallback (2026-05-18)
    'unresolved IndirectGoto',               # emit_function indirect JMP/JML with no resolution (2026-05-29)
    'BRK: software interrupt',               # wrong-width decode or unsupported interrupt opcode
    'COP: software interrupt',               # wrong-width decode or unsupported interrupt opcode
)


def _lint_stubs(out_dir: pathlib.Path) -> list[tuple[str, int, str, str]]:
    """Scan the .c files THIS regen writes for stub markers.

    Generated filenames are game-agnostic now, so the deprecated
    `--prefix` option is intentionally not part of lint selection. Avoid
    scanning stale legacy prefix-named artifacts that are no longer build
    inputs.

    Returns a list of (path, line_no, marker, line_text) tuples. Empty
    list means clean. Caller fails the build if non-empty.
    """
    paths = sorted(out_dir.glob('bank*_v2.c'))
    for filename in ('dispatch_v2.c', 'unresolved_stubs_v2.c'):
        p = out_dir / filename
        if p.exists():
            paths.append(p)
    hits: list[tuple[str, int, str, str]] = []
    for p in paths:
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


def _scan_dirty_variants(results, parsed) -> tuple:
    """Emit-truth attribution: map each per-bank lint marker to the
    (addr24, m, x) variant whose emitted body contains it.

    A variant is 'dirty' iff its OWN body emitted a stub marker — this
    is ground truth for the clean-sibling prune (no graph heuristic).
    Markers in `unresolved_stubs_v2.c` are cross-bank discovery stubs,
    not per-variant bodies, so they never appear in these per-bank
    `src` blobs and are handled separately.

    Returns `(dirty, emitted)` — two sets of (addr24, entry_m, entry_x)
    keys. `emitted` is every variant body seen (clean OR dirty) whose
    base name resolves to a known entry; the prune uses it to find a
    clean SIBLING for auto-promoted functions that have no cfg-declared
    canonical width (canonical_variants is empty for them, so the
    canonical-sibling test can never fire).
    """
    base_start: dict = {}  # (bank, base_name) -> start pc16
    for bank, _p, cfg in parsed:
        for e in cfg.entries:
            bn = e.name or f"bank_{bank:02X}_{e.start & 0xFFFF:04X}"
            base_start[(bank, bn)] = e.start & 0xFFFF
    defre = re.compile(
        r'^RecompReturn\s+([A-Za-z0-9_]+)_M([01])X([01])\(CpuState')
    dirty: set = set()
    emitted: set = set()
    for r in results:
        if r.get('status') != 'ok':
            continue
        bank = r['bank']
        cur = None  # (addr24, m, x) of the body currently being scanned
        for line in r['src'].split('\n'):
            mm = defre.match(line)
            if mm:
                start = base_start.get((bank, mm.group(1)))
                cur = (None if start is None else
                       ((bank << 16) | start, int(mm.group(2)),
                        int(mm.group(3))))
                if cur is not None:
                    emitted.add(cur)
                continue
            if cur is None:
                continue
            for mk in _STUB_MARKERS:
                if mk in line:
                    dirty.add(cur)
                    break
    return dirty, emitted


_VARIANT_DEF_RE = re.compile(
    r'^RecompReturn\s+([A-Za-z0-9_]+)_M([01])X([01])'
    r'\(CpuState\s+\*cpu\)\s*\{')
_SYNTHETIC_BASE_RE = re.compile(r'^bank_([0-9a-fA-F]{2})_([0-9a-fA-F]{4})$')
_GEN_BANK_RE = re.compile(r'^bank([0-9a-fA-F]{2})_v2\.c$')


def _variant_base_pc24(base_name: str, name_to_pc24: dict) -> int | None:
    """Resolve an emitted variant base name back to its 24-bit entry PC."""
    m = _SYNTHETIC_BASE_RE.match(base_name)
    if m:
        return (int(m.group(1), 16) << 16) | int(m.group(2), 16)
    return name_to_pc24.get(base_name)


def _scan_generated_variant_defs(out_dir: pathlib.Path,
                                 name_to_pc24: dict) -> dict:
    """Return variants whose C definitions are actually present in src/gen.

    The trampoline dispatch table must contain fnptrs only for emitted
    symbols. This matters especially for --banks partial regens: skipped
    bank sources stay on disk, while the just-regenerated bank may have a
    narrower surviving variant set. Scanning generated definitions makes
    dispatch_v2.c reflect the build inputs exactly instead of the broader
    in-memory cfg entry set.
    """
    variants: dict[int, set] = {}
    paths = sorted(out_dir.glob('bank*_v2.c'))
    stub_path = out_dir / 'unresolved_stubs_v2.c'
    if stub_path.exists():
        paths.append(stub_path)
    for p in paths:
        try:
            with p.open('r', encoding='utf-8', errors='replace') as f:
                for raw in f:
                    m = _VARIANT_DEF_RE.match(raw)
                    if not m:
                        continue
                    pc24 = _variant_base_pc24(m.group(1), name_to_pc24)
                    if pc24 is None:
                        continue
                    variants.setdefault(pc24, set()).add(
                        (int(m.group(2)), int(m.group(3))))
        except OSError as e:
            print(f"  dispatch scan: failed to read {p}: {e}",
                  file=sys.stderr)
    return variants


def _build_name_to_pc24(parsed) -> dict:
    """Return the generated-symbol base-name -> cfg PC mapping."""
    name_to_pc24: dict[str, int] = {}
    for bank2, _cfg_path2, cfg2 in parsed:
        for entry in cfg2.entries:
            if not entry.name:
                continue
            pc24 = (bank2 << 16) | (entry.start & 0xFFFF)
            name_to_pc24.setdefault(entry.name, pc24)
    return name_to_pc24


def _scan_skipped_bank_valid_variants(out_dir: pathlib.Path, parsed,
                                      only_banks: set | None) -> dict:
    """Seed codegen survivor sets from generated sources for skipped banks.

    Partial regen preserves bank sources outside --banks. Calls emitted by the
    regenerated bank must therefore target the variant symbols that are
    actually present on disk for preserved banks, not every variant currently
    present in the in-memory cfg clone set.
    """
    if only_banks is None:
        return {}
    name_to_pc24 = _build_name_to_pc24(parsed)
    disk_variants = _scan_generated_variant_defs(out_dir, name_to_pc24)
    out: dict[int, frozenset] = {}
    for pc24, mxs in disk_variants.items():
        if ((pc24 >> 16) & 0xFF) in only_banks or not mxs:
            continue
        out[pc24] = frozenset(mxs)
    return out


def _read_skipped_bank_results(out_dir: pathlib.Path,
                               only_banks: set | None) -> list[dict]:
    """Load generated source blobs for banks preserved by --banks.

    Partial regen must treat these on-disk sources as live link inputs:
    their direct references can protect regenerated-bank targets from the
    prune, and their emitted variants count as available for reference-taint.
    """
    if only_banks is None:
        return []
    results: list[dict] = []
    for p in sorted(out_dir.glob('bank*_v2.c')):
        m = _GEN_BANK_RE.match(p.name)
        if not m:
            continue
        bank = int(m.group(1), 16)
        if bank in only_banks:
            continue
        try:
            src = p.read_text(encoding='utf-8', errors='replace')
        except OSError as e:
            print(f"  partial scan: failed to read {p}: {e}",
                  file=sys.stderr)
            continue
        results.append({'status': 'ok', 'bank': bank, 'src': src})
    return results


def _emit_dispatch_table(out_dir: pathlib.Path, parsed,
                         cumulative_pruned: set,
                         inline_arg_map: dict | None = None) -> None:
    """Emit dispatch_v2.c from generated C definitions on disk."""
    name_for_pc24: dict[int, str] = {}
    name_to_pc24 = _build_name_to_pc24(parsed)
    for bank2, _cfg_path2, cfg2 in parsed:
        for entry in cfg2.entries:
            if not entry.name:
                continue
            pc24 = (bank2 << 16) | (entry.start & 0xFFFF)
            name_for_pc24[pc24] = entry.name

    def _disp_name(pc24: int) -> str:
        n = name_for_pc24.get(pc24)
        if n is not None:
            return n
        bank = (pc24 >> 16) & 0xFF
        pc = pc24 & 0xFFFF
        return f"bank_{bank:02X}_{pc:04X}"

    disp_variants = _scan_generated_variant_defs(out_dir, name_to_pc24)
    for (addr, em, ex) in cumulative_pruned:
        s = disp_variants.get(addr)
        if s is not None:
            s.discard((em & 1, ex & 1))
    sorted_pc24s = sorted(pc24 for pc24, mxs in disp_variants.items() if mxs)

    disp_path = out_dir / 'dispatch_v2.c'
    disp_lines = [
        '/* Auto-generated by snesrecomp v2 v2_regen. Do NOT hand-edit.',
        ' *',
        ' * PEI-trampoline dispatch table - runtime cpu_dispatch_pc() looks',
        ' * up function entries here when an RTS/RTL on a trampoline-flagged',
        ' * function hits the unbalanced-cpu->S branch in _emit_return.',
        ' *',
        ' * Sorted by pc24 for binary search. variant[] holds fnptrs for',
        ' * (M0X0, M0X1, M1X0, M1X1) - NULL when that variant was not emitted.',
        ' */',
        '',
        '#include "cpu_state.h"',
        '',
    ]
    fwd_seen: set[str] = set()
    for pc24 in sorted_pc24s:
        base = _disp_name(pc24)
        for em in (0, 1):
            for ex in (0, 1):
                if (em, ex) not in disp_variants[pc24]:
                    continue
                name = f"{base}_M{em}X{ex}"
                if name in fwd_seen:
                    continue
                fwd_seen.add(name)
                disp_lines.append(f"RecompReturn {name}(CpuState *cpu);")
    disp_lines.append('')
    disp_lines.append('const DispatchEntry g_dispatch_table[] = {')
    if not sorted_pc24s:
        disp_lines.append(
            "    { 0xFFFFFFu, { NULL, NULL, NULL, NULL } },  /* sentinel - empty cfg */"
        )
    for pc24 in sorted_pc24s:
        base = _disp_name(pc24)
        inline_n = (inline_arg_map or {}).get(
            pc24, (inline_arg_map or {}).get(pc24 ^ 0x800000, 0))
        slots = ['NULL', 'NULL', 'NULL', 'NULL']
        for em, ex in disp_variants[pc24]:
            idx = ((em & 1) << 1) | (ex & 1)
            slots[idx] = f"{base}_M{em & 1}X{ex & 1}"
        disp_lines.append(
            f"    {{ 0x{pc24:06X}u, {{ {', '.join(slots)} }}, {inline_n} }},  "
            f"/* {base} */"
        )
    disp_lines.append('};')
    disp_lines.append('')
    disp_lines.append(
        f"const unsigned g_dispatch_table_count = "
        f"(unsigned)(sizeof(g_dispatch_table) / sizeof(g_dispatch_table[0]));"
    )
    disp_lines.append('')
    write_if_changed(disp_path, '\n'.join(disp_lines) + '\n')
    print(f"  emitted dispatch table with {len(sorted_pc24s)} entries -> {disp_path}")


def _compute_prunable(dirty_variants: set, emitted_variants: set,
                      canonical_variants: dict,
                      protected_variants: set | None = None) -> set:
    """Decide which dirty (addr24, m, x) variants the emit-truth prune
    may drop, given the cumulative dirty + emitted variant sets and the
    cfg-declared canonical widths.

    A dirty variant is prunable when it is provably a wrong-width clone
    that is never validly reached:

    - It is NOT the (effective) canonical (the un-suffixed alias binds to
      the canonical, so canonicals are never dropped — even when dirty).
    - Its effective-canonical width is EMITTED and CLEAN — the canonical
      proves the bytes are real code at that width, so the dirty clone is
      a wrong-width decode (misaligned operands -> phantom branches /
      garbage calls) that can never be validly reached.
    - It is NOT `protected` (see below): a variant that is the target of a
      DIRECT (non-dispatch) reference from a never-pruned body is provably
      reached at that width, so its stub marker is pending dispatch
      residue — NOT wrong-width garbage.

    Effective canonical = the cfg-declared width(s) when the base has a
    cfg entry, else the default 65816 reset width (1, 1). Auto-promoted
    synthetic `bank_XX_YYYY` targets (demanded by an auto-recovered
    JSR/JMP (abs,X) table, no cfg entry) get the (1, 1) default — the
    SAME default a bare `func` directive gets — so their wrong-width
    clones prune the same way cfg-named functions' do. This is sound for
    genuine RAM/computed-dispatch sites: when the genuine dispatch lives
    at the (1, 1) entry width, that canonical is itself dirty, the
    clean-canonical test fails, and NOTHING is pruned — the genuine site
    survives as loud trap residue (matching the oracle baseline) for cfg
    `indirect_dispatch` / `hle_dispatch` follow-up.

    `protected_variants` closes a hole the clean-canonical test alone
    cannot: when the cfg-DEFAULT width (the effective canonical) is itself
    the WRONG-width decode — i.e. the ingester defaulted a genuinely 16-bit
    routine to (1, 1) — the clean-canonical test protects the garbage
    canonical and instead prunes the REAL non-canonical variant. That real
    variant is reached by a DIRECT tail-call/call from a never-pruned body
    (canonically, the fall-through sibling that `REP`s the width before
    the boundary), and that direct reference dangles at link time
    (LNK2019) with no prune able to repair it (canonicals are unprunable).
    The caller passes the set of variants directly referenced by a
    never-pruned body (see `_protected_ref_targets`); they are provably
    reached and must survive. Observed: Super Metroid
    `sub_82BB7F` / `sub_82BBDD` (the baby-Metroid cluster runs under
    HandleGameOverBabyMetroid's `REP #$30`, so the real width is M0X0, the
    cfg-default M1X1 is the garbage decode). 2026-06-08.
    """
    protected_variants = protected_variants or set()
    dirty_mx: dict = {}
    for (addr, em, ex) in dirty_variants:
        dirty_mx.setdefault(addr, set()).add((em, ex))
    emitted_mx: dict = {}
    for (addr, em, ex) in emitted_variants:
        emitted_mx.setdefault(addr, set()).add((em, ex))
    prunable: set = set()
    for (addr, em, ex) in dirty_variants:
        if (addr, em, ex) in protected_variants:
            continue  # directly referenced by a never-pruned body — reached
        canon = canonical_variants.get(addr) or {(1, 1)}
        if (em, ex) in canon:
            continue  # never prune the (effective) canonical variant
        # Require a clean canonical sibling that was actually emitted:
        # the canonical proves real code at its width, so this dirty
        # non-canonical clone is the wrong-width garbage.
        clean_canon = any(
            (c in emitted_mx.get(addr, set()))
            and (c not in dirty_mx.get(addr, set()))
            for c in canon)
        if clean_canon:
            prunable.add((addr, em, ex))
    return prunable


# Matches a function-variant CALL in an emitted body: `Name_MmXx(cpu)`.
# The def line is `Name_MmXx(CpuState *cpu)` so the literal `(cpu)` tail
# keeps defs from matching. Intra-function labels are `goto L_..._MmXx;`
# (no `(cpu)`) and carry no cross-variant link dependency.
_VARIANT_CALL_RE = re.compile(r'([A-Za-z_]\w*)_M([01])X([01])\(cpu\)')
_VARIANT_SYMBOL_RE = re.compile(r'\b([A-Za-z_]\w*)_M([01])X([01])\b')
_SYNTHETIC_NAME_RE = re.compile(r'^bank_([0-9A-Fa-f]{2})_([0-9A-Fa-f]{4})$')
# The runtime-(m,x) dispatch switch — `switch (((cpu->m_flag & 1) << 1)
# | (cpu->x_flag & 1)) { case 0: _r = Foo_M0X0(cpu); ... }` — references
# ALL FOUR widths of its callee and drops invalid-width cases via
# valid_variants (a switch case never dangles). Only DIRECT references
# (tail-calls / resolved single calls, emitted at the caller's own
# inherited width) can dangle, so the reference-taint graph must EXCLUDE
# these `case 0..3:` / `default:` dispatch lines — counting them taints
# every caller of any wrong-width clone and cascades through the whole
# call graph (observed: ~half of all variants tainted).
_MX_DISPATCH_CASE_RE = re.compile(r'^\s*(case\s+[0-3]\s*:|default\s*:)')


def _scan_variant_refs(results, parsed,
                       include_dispatch_cases: bool = False) -> dict:
    """Build the DIRECT cross-variant reference graph from the emitted
    bodies.

    Returns `{(addr24, m, x): set((taddr24, tm, tx), ...)}` — the
    function-variant CALLs each body makes by a DIRECT reference
    (tail-call / resolved single call at the caller's own inherited
    width). Runtime-(m,x) switch cases are excluded (see
    `_MX_DISPATCH_CASE_RE`): they reference every width and would
    cascade taint across the whole call graph, yet never dangle (the
    invalid-width case is dropped by valid_variants).

    Used by the reference-taint prune: a wrong-width caller clone names
    its DIRECT successor at its own (wrong) inherited width; when that
    successor variant was never emitted / pruned, the reference dangles
    (LNK2019), so the caller clone is itself wrong-width and must be
    pruned. Names resolve via cfg `name` directives; synthetic
    `bank_BB_AAAA` targets encode the address. Unresolvable names are
    skipped (conservative: never over-taint).

    `include_dispatch_cases` is only for partial-regeneration link
    preservation: skipped sources are already on disk and their switch cases
    name real symbols, so a regenerated target bank must keep those symbols
    defined even though the cases are not semantic direct refs."""
    name_to_addr: dict = {}
    base_start: dict = {}
    for bank, _p, cfg in parsed:
        for e in cfg.entries:
            if e.name:
                name_to_addr[e.name] = (bank << 16) | (e.start & 0xFFFF)
            bn = e.name or f"bank_{bank:02X}_{e.start & 0xFFFF:04X}"
            base_start[(bank, bn)] = e.start & 0xFFFF

    def _resolve(nm: str):
        m = _SYNTHETIC_NAME_RE.match(nm)
        if m:
            return (int(m.group(1), 16) << 16) | int(m.group(2), 16)
        return name_to_addr.get(nm)

    defre = re.compile(
        r'^RecompReturn\s+([A-Za-z0-9_]+)_M([01])X([01])\(CpuState')
    # The un-suffixed `void <name>(CpuState *cpu)` entry-point aliases that
    # emit_bank appends after every bank's function bodies (one per cfg
    # `name`). Their body is `RecompReturn _r = <name>_MmXx(cpu); ...` — a
    # _VARIANT_CALL_RE hit. The alias def line is NOT a RecompReturn variant
    # def, so without resetting `cur` here the scanner attributes ALL of the
    # alias section's ~one-call-per-named-entry to whatever real variant body
    # was emitted LAST in the bank. That mis-attribution (hundreds of phantom
    # edges, incl. any dirty-stub alias) falsely taints the last body; pruning
    # it shifts "last body" to the next victim and the prune never converges —
    # the SMW/ALttP non-convergence treadmill (diagnosed 2026-06-02). An alias
    # only forwards to its own canonical variant, never a cross-variant tail
    # call, so it contributes no real reference-taint edge: reset `cur`.
    aliasre = re.compile(r'^void\s+[A-Za-z0-9_]+\s*\(CpuState')
    refs: dict = {}
    for r in results:
        if r.get('status') != 'ok':
            continue
        bank = r['bank']
        cur = None
        for line in r['src'].split('\n'):
            mm = defre.match(line)
            if mm:
                start = base_start.get((bank, mm.group(1)))
                cur = (None if start is None else
                       ((bank << 16) | start, int(mm.group(2)),
                        int(mm.group(3))))
                continue
            if aliasre.match(line):
                cur = None  # entry-point alias wrapper — not a variant body
                continue
            if cur is None:
                continue
            if (not include_dispatch_cases
                    and _MX_DISPATCH_CASE_RE.match(line)):
                continue  # runtime-(m,x) switch case — not a direct ref
            for cm in _VARIANT_CALL_RE.finditer(line):
                taddr = _resolve(cm.group(1))
                if taddr is None:
                    continue
                tv = (taddr, int(cm.group(2)), int(cm.group(3)))
                if tv == cur:
                    continue  # self-reference (recursion) is not a taint
                refs.setdefault(cur, set()).add(tv)
    return refs


def _merge_variant_ref_graphs(*graphs: dict) -> dict:
    merged: dict = {}
    for graph in graphs:
        for source, targets in graph.items():
            merged.setdefault(source, set()).update(targets)
    return merged


def _variant_ref_targets(refs: dict) -> set:
    targets: set = set()
    for ts in refs.values():
        targets |= ts
    return targets


def _variant_id(variant) -> str:
    addr, em, ex = variant
    return f"${addr:06X}/M{em}X{ex}"


def _reject_partial_root_prune(candidates: set, link_roots: set,
                               phase: str) -> None:
    """Enforce the preserved-source link contract during partial regen."""
    conflicts = candidates & link_roots
    if not conflicts:
        return
    names = sorted(_variant_id(v) for v in conflicts)
    raise RuntimeError(
        f"partial regeneration attempted to {phase} preserved-bank "
        f"link root(s): {names}")


def _scan_link_variant_targets(results, parsed) -> set:
    """Return every generated variant symbol referenced by ``results``.

    This is deliberately broader than :func:`_scan_variant_refs`.  The
    reference-taint graph needs an owning, decoded function body and excludes
    alias wrappers and runtime dispatch cases for semantic reasons.  A partial
    regeneration has a simpler link-time contract: *every* call expression in
    a preserved source file must still resolve after the selected banks are
    regenerated.  That includes calls made by alias wrappers, synthetic
    callers absent from the cfg, and runtime M/X dispatch cases.

    The broad symbol match also covers function-pointer initializers and
    declarations. Definitions in a preserved bank resolve back to that same
    preserved bank and are harmless; the caller later filters roots to the
    bank or banks selected for regeneration.
    """
    name_to_addr = _build_name_to_pc24(parsed)
    targets: set = set()
    for result in results:
        if result.get('status') != 'ok':
            continue
        for match in _VARIANT_SYMBOL_RE.finditer(result.get('src', '')):
            base = match.group(1)
            synthetic = _SYNTHETIC_NAME_RE.match(base)
            if synthetic:
                addr = ((int(synthetic.group(1), 16) << 16)
                        | int(synthetic.group(2), 16))
            else:
                addr = name_to_addr.get(base)
            if addr is None:
                continue
            targets.add((addr, int(match.group(2)), int(match.group(3))))
    return targets


def _protected_ref_targets(refs: dict, canonical_variants: dict,
                           dirty_variants: set | None = None) -> set:
    """Return DIRECT targets from source bodies that cannot be pruned.

    Canonical and clean non-canonical source variants are never pruned, so
    their DIRECT (non-dispatch) tail-call / single-call references are
    permanent in the linked binary and MUST resolve. The boundary `(m, x)` of
    each such reference is computed from the source body's own decode, so the
    referenced (addr, m, x) is provably reached at that width — even when
    it differs from the target's cfg-default width (the
    HandleGameOverBabyMetroid `REP #$30` fall-through case). Feeding these
    to `_compute_prunable` as `protected` stops the emit-truth /
    reference-taint prune from dropping a genuinely-reached variant whose
    only blemish is a pending indirect-dispatch stub marker.

    Dirty non-canonical sources are excluded: those are exactly the
    wrong-width bodies the prune may remove.

    `refs` is `_scan_variant_refs` output (dispatch-switch cases already
    excluded, so only danglable direct references are present)."""
    dirty_variants = dirty_variants or set()
    protected: set = set()
    for (saddr, sm, sx), targets in refs.items():
        canon = canonical_variants.get(saddr) or {(1, 1)}
        if (sm, sx) in canon or (saddr, sm, sx) not in dirty_variants:
            protected |= targets
    return protected


def _propagate_reference_taint(dirty: set, refs: dict, emitted: set,
                               bank_set: set, pruned: set) -> set:
    """Fixpoint-taint wrong-width caller clones for the reference-taint
    prune. Seed = the emit-truth dirty set (own-body marker). A variant
    V then becomes tainted when it CALLs a target variant T that is:

    - in an in-cfg-set bank yet NOT currently emitted (emitted - pruned)
      -> a guaranteed dangling link reference: T was pruned as a dirty
      wrong-width clone, or was never emitted at that inherited width; or
    - itself tainted -> the wrong-width decode chains deeper.

    Out-of-cfg-set targets are skipped (they get loud
    cpu_trace_unresolved_stub_trap bodies in unresolved_stubs_v2.c, so
    they never dangle). The clean-canonical guard in `_compute_prunable`
    then drops only the NON-canonical tainted clones whose canonical
    width is clean — so a genuine multi-width caller is never pruned
    (its canonical-width successors resolve to emitted variants and the
    canonical itself stays untainted)."""
    tainted = set(dirty)
    emitted_now = emitted - pruned
    changed = True
    while changed:
        changed = False
        for v, targets in refs.items():
            if v in tainted:
                continue
            for (taddr, tm, tx) in targets:
                tbank = (taddr >> 16) & 0xFF
                dangling = (tbank in bank_set
                            and (taddr, tm, tx) not in emitted_now)
                if dangling or (taddr, tm, tx) in tainted:
                    tainted.add(v)
                    changed = True
                    break
    return tainted


def _apply_variant_prune(parsed, cumulative_pruned: set,
                         preserved_valid_variants: dict | None = None) -> dict:
    """Drop every pruned (addr24, m, x) variant's cfg entry and rebuild
    the survivor valid-variants map fed to codegen (also pushed via
    set_valid_variants so the runtime-(m,x) switch stops emitting the
    pruned case). `preserved_valid_variants` carries on-disk survivor
    sets for --banks-skipped sources. Returns the new valid_variants_map.
    Idempotent: safe to call repeatedly as cumulative_pruned grows."""
    prune_by_bank: dict = {}
    for (addr, em, ex) in cumulative_pruned:
        prune_by_bank.setdefault((addr >> 16) & 0xFF, set()).add(
            (addr & 0xFFFF, em, ex))
    for bank2, _p2, cfg2 in parsed:
        pset = prune_by_bank.get(bank2)
        if not pset:
            continue
        cfg2.entries = [
            e for e in cfg2.entries
            if (e.start & 0xFFFF, e.entry_m & 1, e.entry_x & 1)
            not in pset]
    preserved_valid_variants = preserved_valid_variants or {}
    preserved_addrs = set(preserved_valid_variants)
    vvm: dict = {a: set(s) for a, s in preserved_valid_variants.items()}
    for bank2, _p2, cfg2 in parsed:
        for e in cfg2.entries:
            a = (bank2 << 16) | (e.start & 0xFFFF)
            if a in preserved_addrs:
                continue
            vvm.setdefault(a, set()).add((e.entry_m & 1, e.entry_x & 1))
    vvm = {a: frozenset(s) for a, s in vvm.items()}
    set_valid_variants(vvm)
    return vvm


def _rebuild_callee_exit_mx(parsed, variants: dict) -> tuple:
    """Build the decoder's callee exit-(m, x) lookup from cfg state.

    `autoroute_exit_mx` writes per-variant routes back into each BankCfg,
    while hand-authored `exit_mx_at` entries are broadcast to every
    discovered entry variant. Keep this in one place so initial setup and
    post-auto-promote refreshes cannot drift.
    """
    callee_exit_mx: dict = {}
    cfg_exit_mx_count = 0
    declared_exit_mx: dict = {}

    for _bank, _cfg_path, cfg in parsed:
        for (b_id, addr16, m_val, x_val) in cfg.exit_mx_at:
            declared_exit_mx[(b_id & 0xFF, addr16 & 0xFFFF)] = (
                m_val, x_val)

    for (b_id, addr16), (ex_m, ex_xf) in declared_exit_mx.items():
        target_pc24 = (b_id << 16) | addr16
        mx_set = variants.get(target_pc24)
        if mx_set:
            for em, ex2 in mx_set:
                callee_exit_mx[(target_pc24, em, ex2)] = (ex_m, ex_xf)
                cfg_exit_mx_count += 1
        else:
            # No discovered variants yet: preserve the historical cfg
            # default so early decode still gets the annotation.
            callee_exit_mx[(target_pc24, 1, 1)] = (ex_m, ex_xf)
            cfg_exit_mx_count += 1

    per_variant_count = 0
    for _bank, _cfg_path, cfg in parsed:
        for (b_id, addr16, em_in, ex_in, ex_m, ex_xf) in \
                cfg.exit_mx_at_per_variant:
            target_pc24 = ((b_id & 0xFF) << 16) | (addr16 & 0xFFFF)
            callee_exit_mx[(target_pc24, em_in & 1, ex_in & 1)] = (
                ex_m & 1, ex_xf & 1)
            per_variant_count += 1

    return (
        callee_exit_mx,
        cfg_exit_mx_count,
        len(declared_exit_mx),
        per_variant_count,
    )


def _discover_variants_from_current_entries(parsed, rom: bytes, variants: dict,
                                            decoded: set, dispatch_helpers,
                                            all_data_regions=None,
                                            pruned_variants=None,
                                            pending_entries=None,
                                            callee_exit_mx=None,
                                            callee_exit_mx_modes=None,
                                            ) -> int:
    """Extend variant discovery after auto-promotion has added entries.

    The initial variant-discovery pass can see calls *to* synthetic
    targets, but cannot scan calls *inside* those targets until the
    auto-promote loop has created BankEntry records for them.

    The same refresh is also needed after exit-(M, X) autorouting learns
    that a callee returns with different status widths. A call site after
    such a JSR/JSL may demand a different target variant than the
    pre-autoroute decode discovered. If that target variant is missing,
    later valid-variant maps can suppress a live runtime mode and the
    generated switch falls through to its default no-op case.

    Mutates ``variants`` / ``decoded`` and appends missing BankEntry
    clones for already-known function addresses.  Returns the number of
    entries appended.
    """
    pruned_variants = pruned_variants or set()

    addr_to_end: dict[int, "Optional[int]"] = {}
    addr_to_bank: dict[int, int] = {}
    entries_by_addr: dict[int, object] = {}
    for bank, _cfg_path, cfg in parsed:
        for entry in cfg.entries:
            addr = (bank << 16) | (entry.start & 0xFFFF)
            addr_to_end.setdefault(addr, entry.end)
            addr_to_bank.setdefault(addr, bank)
            entries_by_addr.setdefault(addr, entry)

    def _in_data_region(target: int) -> bool:
        if not all_data_regions:
            return False
        tbank = (target >> 16) & 0xFF
        tpc = target & 0xFFFF
        return any(
            (b & 0xFF) == tbank and
            (s & 0xFFFF) <= tpc < (e & 0xFFFF)
            for (b, s, e) in all_data_regions)

    queue: list[tuple[int, int, int, int, "Optional[int]"]] = []
    if pending_entries is None:
        seed = {
            ((bank << 16) | (entry.start & 0xFFFF),
             entry.entry_m & 1, entry.entry_x & 1)
            for bank, _cfg_path, cfg in parsed
            for entry in cfg.entries
        }
    else:
        seed = set(pending_entries)

    for addr, em, ex in sorted(seed):
        if (addr, em, ex) in pruned_variants or addr not in addr_to_end:
            continue
        variants.setdefault(addr, set()).add((em, ex))
        if (addr, em, ex) not in decoded:
            bank = addr_to_bank[addr]
            entry = entries_by_addr[addr]
            queue.append((bank, entry.start, em, ex, addr_to_end.get(addr)))

    while queue:
        bank, start, em, ex, end = queue.pop()
        addr = (bank << 16) | (start & 0xFFFF)
        if (addr, em, ex) in decoded or (addr, em, ex) in pruned_variants:
            continue
        decoded.add((addr, em, ex))
        try:
            graph = decode_function(rom, bank, start,
                                    entry_m=em, entry_x=ex,
                                    end=end,
                                    dispatch_helpers=dispatch_helpers,
                                    data_regions=all_data_regions or None,
                                    callee_exit_mx=callee_exit_mx,
                                    callee_exit_mx_modes=callee_exit_mx_modes)
        except Exception:
            continue
        for di in graph.insns.values():
            ins = di.insn
            if ins.mnem == 'JSR' and ins.length == 3:
                src_bank = (ins.addr >> 16) & 0xFF
                target = ((src_bank << 16) | (ins.operand & 0xFFFF))
            elif ins.mnem == 'JSL':
                target = ins.operand & 0xFFFFFF
            elif ins.mnem == 'JMP' and ins.length == 4:
                target = ins.operand & 0xFFFFFF
            else:
                target = None
            if target is not None and _in_data_region(target):
                target = None
            if target is not None:
                em2 = ins.m_flag & 1
                ex2 = ins.x_flag & 1
                if (target, em2, ex2) not in pruned_variants:
                    if (em2, ex2) not in variants.setdefault(target, set()):
                        variants[target].add((em2, ex2))
                    if target in addr_to_end and (target, em2, ex2) not in decoded:
                        tb = addr_to_bank[target]
                        ts = target & 0xFFFF
                        queue.append((tb, ts, em2, ex2, addr_to_end[target]))
            for d_target in getattr(ins, 'dispatch_entries', None) or []:
                if d_target == 0:
                    continue
                if getattr(ins, 'dispatch_kind', 'short') == 'long':
                    d_addr = d_target & 0xFFFFFF
                else:
                    d_addr = ((ins.addr >> 16) & 0xFF) << 16 | (
                        d_target & 0xFFFF)
                if _in_data_region(d_addr):
                    continue
                em2 = ins.m_flag & 1
                ex2 = ins.x_flag & 1
                if (d_addr, em2, ex2) in pruned_variants:
                    continue
                if (em2, ex2) not in variants.setdefault(d_addr, set()):
                    variants[d_addr].add((em2, ex2))
                if d_addr in addr_to_end and (d_addr, em2, ex2) not in decoded:
                    tb = addr_to_bank[d_addr]
                    ts = d_addr & 0xFFFF
                    queue.append((tb, ts, em2, ex2, addr_to_end[d_addr]))

    added = 0
    for bank, _cfg_path, cfg in parsed:
        current_keys = {
            ((bank << 16) | (e.start & 0xFFFF), e.entry_m & 1, e.entry_x & 1)
            for e in cfg.entries
        }
        by_pc: dict[int, object] = {}
        for e in cfg.entries:
            by_pc.setdefault(e.start & 0xFFFF, e)
        for addr, mxs in sorted(variants.items()):
            if ((addr >> 16) & 0xFF) != bank:
                continue
            base = by_pc.get(addr & 0xFFFF)
            if base is None:
                continue
            for em, ex in sorted(mxs):
                key = (addr, em, ex)
                if key in current_keys or key in pruned_variants:
                    continue
                cfg.entries.append(BankEntry(
                    name=base.name,
                    start=base.start,
                    end=base.end,
                    entry_m=em,
                    entry_x=ex,
                ))
                current_keys.add(key)
                added += 1
    return added


def _emit_bank_one(args_dict: dict) -> dict:
    """Worker function: emit one bank end-to-end and return all outputs.

    Defined at module level so multiprocessing.Pool can pickle it on
    Windows (spawn start method). Re-applies codegen globals from
    args_dict on every call — worker processes do not share state with
    main, and per-pass dynamic state (name_map / trampoline_returns /
    force_variant_at) can change between calls within one worker.

    Sequential path (--jobs 1) calls this directly without pickling.
    Parallel path (--jobs >1) submits via Pool.map; each work item is
    self-contained so worker processes have everything they need.

    Returns a dict with `status: 'ok'|'fail'`, the emitted source, and
    every per-bank collector list plus drained codegen globals
    (`take_*` results). Main merges these into the global accumulators
    after the pass."""
    # Spawned workers persist across banks and passes, but each work item owns
    # a distinct pickled analysis snapshot.  Bound the cache to one bank task:
    # this retains useful repeated decodes inside emit_bank without retaining
    # graphs that cannot hit in the next task.
    clear_decode_cache()
    set_decode_cache_enabled(args_dict.get('decode_cache_enabled', True))
    set_rom_size(args_dict['rom_size'])
    set_name_resolver(args_dict['name_map'])
    set_force_variant_at(args_dict['force_variant_at'])
    set_valid_variants(args_dict.get('valid_variants') or {})
    set_trampoline_returns(args_dict['trampoline_returns'])
    set_active_inline_arg_map(args_dict.get('inline_arg_map') or None)

    bank = args_dict['bank']
    cfg = args_dict['cfg']
    rom = args_dict['rom']
    cfg_bank_field = getattr(cfg, 'bank', bank)
    bank_field_warning = None
    if cfg_bank_field != bank:
        bank_field_warning = (
            f"  bank{cfg_bank_field:02X}.cfg: bank field "
            f"${cfg_bank_field:02X} doesn't match filename "
            f"${bank:02X}; using filename")

    bank_suppressed: list = []
    bank_const_z_folds: list = []
    bank_dispatch_suppressed: list = []
    bank_unresolved_indirects: list = []

    try:
        src = emit_bank(rom, bank=bank, entries=cfg.entries,
                        inline_arg_map=args_dict.get('inline_arg_map') or None,
                        dispatch_helpers=args_dict['dispatch_helpers'],
                        indirect_call_tables=getattr(
                            cfg, 'indirect_call_tables', None),
                        indirect_dispatch=args_dict['indirect_dispatch_map'],
                        suppressed_collector=bank_suppressed,
                        const_z_fold_collector=bank_const_z_folds,
                        dispatch_target_suppressed_collector=
                            bank_dispatch_suppressed,
                        unresolved_indirect_collector=
                            bank_unresolved_indirects,
                        data_regions=cfg.data_regions or None,
                        exclude_ranges=cfg.exclude_ranges or None,
                        callee_exit_mx=args_dict['callee_exit_mx'],
                        callee_exit_mx_modes=args_dict['callee_exit_mx_modes'],
                        hle_spc_upload=getattr(
                            cfg, 'hle_spc_upload', None) or None,
                        hle_func=getattr(
                            cfg, 'hle_func', None) or None,
                        hle_dispatch=getattr(
                            cfg, 'hle_dispatch', None) or None)
    except Exception as e:
        return {
            'bank': bank,
            'status': 'fail',
            'error': f"{type(e).__name__}: {e}",
            'traceback': traceback.format_exc(),
            'bank_field_warning': bank_field_warning,
        }

    return {
        'bank': bank,
        'status': 'ok',
        'src': src,
        'cfg_entries_count': len(cfg.entries),
        'suppressed': bank_suppressed,
        'const_z_folds': bank_const_z_folds,
        'dispatch_suppressed': bank_dispatch_suppressed,
        'unresolved_indirects': bank_unresolved_indirects,
        'unresolved_calls': take_unresolved_call_targets(),
        'rejected_call_targets': take_rejected_call_targets(),
        'trampoline_returns_local': take_trampoline_returns(),
        'bank_field_warning': bank_field_warning,
    }


def main() -> int:
    p = argparse.ArgumentParser(description="v2 regen — emit one C file per bank cfg")
    p.add_argument('--rom', required=True, help='Path to game ROM file (.sfc)')
    p.add_argument('--cfg-dir', required=True,
                   help='Directory containing bankXX.cfg files')
    p.add_argument('--out-dir', required=True,
                   help='Output directory for emitted C files')
    # (Bank/dispatch filenames are game-agnostic: bankXX_v2.c / dispatch_v2.c.
    #  --prefix is accepted but ignored for backward compatibility with older
    #  per-game invocations.)
    p.add_argument('--prefix', default='', help=argparse.SUPPRESS)
    p.add_argument('--banks', default=None,
                   help='Comma-separated hex bank IDs to (re)emit. Other '
                        'banks keep their existing .c files on disk. Use '
                        'for fast iteration on codegen changes that only '
                        'affect intra-bank emit (e.g. dispatch shape). '
                        'Cross-bank demands still drive autopromote, but '
                        'banks outside this filter are NOT written. '
                        'Example: --banks 07 (only bank 7); --banks 00,07.')
    p.add_argument('--no-decode-cache', action='store_true',
                   help='Disable the decode_function memoization cache. '
                        'Cache is on by default and cleared between each '
                        'pipeline phase to bound memory. Use this flag '
                        'to bisect output divergence the cache might '
                        'introduce.')
    p.add_argument('--timeout-seconds', type=int, default=5400,
                   help='DEPRECATED / ignored. The regen wall-clock cap '
                        'is FIXED at 5400s (1.5 h) and is not '
                        'configurable — the per-call values we kept '
                        'passing were always too short. This flag is '
                        'retained only so existing callers do not error; '
                        'its value has no effect.')
    # Default --jobs is read from SNESRECOMP_JOBS env var (matching the
    # SNESRECOMP_TRACE convention in the runner). Hardcoded fallback is
    # 1 (sequential) — safe for low-to-mid-end machines and preserves
    # output bit-identicality against the pre-parallel pipeline.
    # Power users set the env var once (e.g. `setx SNESRECOMP_JOBS 8`
    # on a high-core desktop) and forget.
    _env_jobs = os.environ.get('SNESRECOMP_JOBS', '').strip()
    try:
        _default_jobs = int(_env_jobs) if _env_jobs else 1
    except ValueError:
        print(f"  WARN: SNESRECOMP_JOBS={_env_jobs!r} is not an integer; "
              f"defaulting to 1", file=sys.stderr)
        _default_jobs = 1
    p.add_argument('--jobs', type=int, default=_default_jobs,
                   help='Parallel worker count for per-bank emit. '
                        'Reads SNESRECOMP_JOBS env var as default '
                        '(currently: {}); hardcoded fallback is 1. '
                        'Set to N to spread emit across N processes '
                        'via multiprocessing.Pool. Map to physical '
                        'cores for CPU-bound work (e.g. 8 on an '
                        '8C/16T desktop); hyperthreads rarely help '
                        'and compete for execution units.'.format(
                            _default_jobs))
    p.add_argument('--tier-down-stubs', action='store_true',
                   help='Opt-in (bring-up): emit cross-ROM-bank unresolved-'
                        'function stubs (unresolved_stubs_v2.c) as interpreter '
                        'tier-downs that RUN the real ROM bytes, instead of the '
                        'no-op cpu_trace_unresolved_stub_trap. A bail falls back '
                        'to the same stack-safe abandon (never worse), and every '
                        'fire is recorded to the Tier-2 gap manifest. Shipped '
                        'titles stay strict (default off). See docs/MULTI_TIER.md '
                        'Phase 4.')
    args = p.parse_args()

    # ── Phase-tracking + watchdog ───────────────────────────────────
    regen_start_time = time.time()

    def _phase(name: str) -> None:
        """Mark a pipeline phase boundary: print elapsed time and
        clear the decode cache to release the previous phase's memory.
        Pre-emit phases pass different kwarg permutations to
        decode_function, so the same (entry, m, x) gets cached under
        N keys across N phases — unbounded memory if not cleared."""
        elapsed = time.time() - regen_start_time
        stats = decode_cache_stats()
        print(f"[{elapsed:7.1f}s] {name} "
              f"(cache before clear: {stats['size']} entries, "
              f"{stats['hits']}h/{stats['misses']}m)",
              flush=True)
        clear_decode_cache()

    # Regen wall-clock cap: FIXED at 5400s (1.5 h), always armed, NOT
    # configurable. History: the cap was a `--timeout-seconds` knob
    # (default 1800s) that every caller had to override and still got
    # wrong — MMX alone blows past 30 min, so 1800s killed real regens
    # mid-pipeline. Hardcoding a generous 1.5 h removes the footgun: you
    # never pass a timeout, and it's never too short for a healthy regen.
    # A run that genuinely needs >1.5 h is a red flag (runaway/looping),
    # which is exactly when we WANT the watchdog to fire. (`args.
    # timeout_seconds` is parsed but ignored — see the flag's help.)
    timeout_sec = 5400

    def _on_timeout() -> None:
        elapsed = time.time() - regen_start_time
        stats = decode_cache_stats()
        print(f"\n!!! v2_regen TIMEOUT after {elapsed:.0f}s "
              f"(fixed 1.5 h cap = {timeout_sec}s) !!!",
              file=sys.stderr, flush=True)
        print(f"!!! last phase cache: {stats} !!!",
              file=sys.stderr, flush=True)
        os._exit(124)

    watchdog = threading.Timer(timeout_sec, _on_timeout)
    watchdog.daemon = True
    watchdog.start()

    set_decode_cache_enabled(not args.no_decode_cache)
    only_banks: set | None = None
    if args.banks:
        only_banks = set()
        for tok in args.banks.split(','):
            tok = tok.strip()
            if not tok:
                continue
            only_banks.add(int(tok, 16))

    rom = load_rom(args.rom)
    set_rom_size(len(rom))
    cfg_dir = pathlib.Path(args.cfg_dir)
    target_out_dir = pathlib.Path(args.out_dir)

    cfgs = sorted(cfg_dir.glob('bank*.cfg'))
    if not cfgs:
        print(f"v2_regen: no bank*.cfg under {cfg_dir}", file=sys.stderr)
        return 2

    output_workspace = _AtomicOutputDir(target_out_dir)
    out_dir = output_workspace.staging
    assert out_dir is not None
    print(f"  staging generated output in {out_dir}")

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
    _phase("autoroute_wrappers")
    print("Auto-routing SMW DB-transition wrappers...")
    wrapper_fixes = autoroute_wrappers(parsed, name_map, cross_bank_names, rom)
    print(format_fix_summary(wrapper_fixes))

    # Auto-detect tail-call fallthrough cfg sites. Pattern: cfg `func A
    # end:<pc>` whose <pc> is also the start of cfg `func B`, AND A's
    # last decoded instruction is a non-terminal that falls through to
    # exactly <pc>. emit_function would otherwise emit an unresolvable
    # goto at the boundary. See `recompiler/v2/tail_call_autoroute.py`.
    _phase("autoroute_tail_calls")
    print("Auto-detecting tail-call fallthrough sites...")
    tail_call_fixes = autoroute_tail_calls(parsed, rom)
    print(format_tail_call_summary(tail_call_fixes))

    # Auto-detect PHA-RTS dispatch sites. Pattern (instruction-aligned
    # inside any decoded function body):
    #   LDA $abs,Y / DEC A / PHA / SEP #$30 / RTS
    # The PHA leaves a (handler-1) on the stack; the trailing RTS pops
    # and adds 1, dispatching into the loaded function pointer. Without
    # an `indirect_dispatch` directive the recompiler emits the PHA as a
    # literal stack push, which the next RTS in the caller chain pops
    # as a bogus return address — DB/PB end up at random banks. Class
    # fix synthesises the directive for every site the byte pattern
    # matches inside cfg-declared function bodies. See
    # `recompiler/v2/pha_rts_autoroute.py`.
    _phase("autoroute_pha_rts")
    print("Auto-detecting PHA-RTS dispatch sites...")
    pha_rts_fixes = autoroute_pha_rts(parsed, rom)
    print(format_pha_rts_summary(pha_rts_fixes))

    # Auto-detect dispatch helpers BEFORE exit-(M, X) autoroute. The
    # autoroute decoder needs `dispatch_helpers` to recognise SMW's
    # `JSL <helper>; <data table>` pattern as a dispatch terminator —
    # otherwise the bytes after the JSL are decoded as garbage
    # instructions, the function ends with no RTS/RTL/RTI, and
    # `analyze_function_exit_mx` returns ambiguous. The non-leaf
    # auto-router needs this signal to route exits for dispatch-
    # terminator functions like `BufferScrollingTiles_Layer1_Init`.
    # (Detection is identical to the existing block; it just moved up.)
    _phase("dispatch_helper_discovery")
    print("Auto-detecting JSL dispatch helpers...")
    dispatch_helpers: dict = {}
    jsl_targets: set = set()
    call_targets: set = set()   # every JSR/JSL target (for inline-arg scan)
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
                    call_targets.add(ins.operand & 0xFFFFFF)
                elif ins.mnem == 'JMP' and ins.length == 4:
                    jsl_targets.add(ins.operand & 0xFFFFFF)
                elif ins.mnem == 'JSR' and ins.length == 3:
                    call_targets.add((bank << 16) | (ins.operand & 0xFFFF))
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

    # Auto-detect JSR/JSL INLINE-ARGUMENT routines (no cfg hint — the
    # byte count is a property of the callee's own code; see
    # detect_inline_arg_bytes / PRINCIPLES "hints are not correctness").
    # Such a callee reads its return address off the stack, consumes the
    # N bytes after the call site as a parameter, and advances the stacked
    # return address by N. The decoder must skip those N bytes at every
    # call site (see _labeled_successors) or it runs into the inline data
    # and misdecodes it as code. Probe each call target at both common
    # entry widths (the ADC #imm that encodes N is m-dependent, so the
    # idiom only resolves at the routine's real width); accept only a
    # unique result. Canonical case: Super Metroid APU_UploadBankIP
    # ($80:800A), a 3-byte inline ROM pointer the reset embeds after its
    # JSL — without this, I_RESET decodes the pointer as BRK and returns
    # early (black screen).
    _phase("inline_arg_discovery")
    print("Auto-detecting JSR/JSL inline-argument routines...")
    # Transitive closure of call targets. The initial scan above only saw
    # the immediate instructions of cfg entries; an inline-arg callee can
    # sit deeper — e.g. Super Metroid's APU_UploadBankIP ($80:800A) is
    # reached via I_RESET -> cross-bank JMP $80:8423 -> JSL $80:800A, and
    # $8423 is not a cfg entry until auto-promote (which runs later). Walk
    # JSL/JSR/JMP-long from every cfg entry to a fixpoint, decoding each
    # function once (default width suffices — we only need the call-target
    # operands, captured before any inline-data truncation) so every
    # transitively-reachable callee is probed.
    _wl: list = []
    for bank, _cfg_path, cfg in parsed:
        for entry in cfg.entries:
            _wl.append(((bank << 16) | (entry.start & 0xFFFF),
                        entry.entry_m & 1, entry.entry_x & 1))
    _decoded_for_calls: set = set()
    while _wl:
        pc24, em, ex = _wl.pop()
        if pc24 in _decoded_for_calls:
            continue
        _decoded_for_calls.add(pc24)
        dbank, daddr = (pc24 >> 16) & 0xFF, pc24 & 0xFFFF
        try:
            g = decode_function(rom, dbank, daddr, entry_m=em, entry_x=ex)
        except Exception:
            continue
        for di in g.insns.values():
            ins = di.insn
            if ins.mnem == 'JSL' or (ins.mnem == 'JMP' and ins.length == 4):
                t = ins.operand & 0xFFFFFF
            elif ins.mnem == 'JSR' and ins.length == 3:
                t = (dbank << 16) | (ins.operand & 0xFFFF)
            else:
                continue
            call_targets.add(t)
            if t not in _decoded_for_calls:
                _wl.append((t, 1, 1))
    inline_arg_map: dict = {}
    for tgt in sorted(call_targets):
        tbank = (tgt >> 16) & 0xFF
        taddr = tgt & 0xFFFF
        seen_n: set = set()
        for em, ex in ((0, 0), (1, 1)):
            n = detect_inline_arg_bytes(rom, tbank, taddr, em, ex)
            if n:
                seen_n.add(n)
        if len(seen_n) == 1:
            inline_arg_map[tgt] = seen_n.pop()
    if inline_arg_map:
        print(f"  detected {len(inline_arg_map)} inline-argument routine(s):")
        for tgt, n in sorted(inline_arg_map.items()):
            nm = name_map.get(tgt, name_map.get(tgt ^ 0x800000, ''))
            print(f"    ${tgt:06X} consumes {n} inline byte(s)"
                  f"{(' (' + nm + ')') if nm else ''}")
    else:
        print(f"  none (scanned {len(call_targets)} call targets)")
    # Install as the process-wide default so every main-process decode
    # (variant discovery, exit-mx, etc.) skips inline-arg bytes. The
    # multiprocessing emit workers receive it via the work-item dict and
    # set it in their own process (see _emit_bank_one).
    set_active_inline_arg_map(inline_arg_map)

    # Auto-detect leaf-function exit-(M, X) state mutations. Pattern:
    # cfg `func F` whose decoded body has NO JSR/JSL inside AND whose
    # RTS/RTL terminators all exit with the same (M, X) != entry (M, X)
    # — typically a small SEP/REP-then-RTS leaf. The decoder otherwise
    # assumes callees preserve (M, X), miscoding callers' post-call
    # operand widths. See `recompiler/v2/exit_mx_autoroute.py` for why
    # this is leaf-only (the unrestricted fixpoint version regressed
    # GraphicsDecompress on 2026-05-03 and was reverted to opt-in).
    _phase("autoroute_exit_mx")
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

    # Collect `force_variant_at` directives across every bank cfg and
    # install them as a single keyed-by-site-PC24 map. Codegen consults
    # this in _emit_call to pin a hardcoded variant at the named site
    # (diagnostic for suspected m-flag tracking bugs). One global map
    # keyed by 24-bit site PC; per-bank cfgs contribute non-overlapping
    # entries (parser rejects duplicates within a single cfg).
    force_variant_map: dict = {}
    for _bank, _cfg_path, _cfg in parsed:
        for site_pc24, (m_val, x_val) in _cfg.force_variant_at.items():
            if site_pc24 in force_variant_map:
                print(
                    f"v2_regen: WARNING: force_variant_at duplicate "
                    f"site ${site_pc24:06X} across cfgs (keeping first)",
                    file=sys.stderr)
                continue
            force_variant_map[site_pc24] = (m_val, x_val)
    set_force_variant_at(force_variant_map)
    if force_variant_map:
        print(
            f"v2_regen: force_variant_at active at {len(force_variant_map)} "
            f"site(s):")
        for site_pc24, (m_val, x_val) in sorted(force_variant_map.items()):
            print(f"  ${site_pc24:06X} -> M{m_val}X{x_val}")

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

    def _build_callee_exit_mx_modes(callee_map: dict) -> dict:
        """Collect multi-exit callee mode sets for dynamic post-call decode."""
        from v2.decoder import (
            analyze_function_exit_mx_modes,
            decode_function as _decode_function,
        )
        mode_map: dict = {}
        for b_id, _cfg_path2, cfg2 in parsed:
            for entry2 in cfg2.entries:
                target_pc24 = (b_id << 16) | (entry2.start & 0xFFFF)
                key = (target_pc24, entry2.entry_m & 1, entry2.entry_x & 1)
                try:
                    graph2 = _decode_function(
                        rom, b_id, entry2.start,
                        entry_m=entry2.entry_m,
                        entry_x=entry2.entry_x,
                        end=entry2.end,
                        dispatch_helpers=dispatch_helpers,
                        data_regions=all_data_regions or None,
                        callee_exit_mx=callee_map,
                    )
                except Exception:
                    continue
                modes = analyze_function_exit_mx_modes(graph2, callee_map)
                if modes and len(modes) > 1:
                    mode_map[key] = frozenset((m & 1, x & 1) for (m, x) in modes)
        if mode_map:
            print(f"  collected {len(mode_map)} ambiguous callee exit-mode sets")
        return mode_map

    _phase("variant_discovery")
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

    # Capture cfg-declared (canonical) variants BEFORE the clone step.
    # These are the hand-verified entry widths; the emit-truth variant
    # prune pass NEVER prunes them (the un-suffixed alias binds to the
    # canonical, and a clean canonical is exactly what proves a
    # wrong-width clone is the unreachable garbage variant). Keyed by
    # 24-bit entry address -> set of (m, x).
    canonical_variants: dict[int, set] = {}
    for bank, _cfg_path, cfg in parsed:
        for entry in cfg.entries:
            addr = (bank << 16) | (entry.start & 0xFFFF)
            canonical_variants.setdefault(addr, set()).add(
                (entry.entry_m & 1, entry.entry_x & 1))

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
            if (addr, decl_mx) in seen_keys:
                continue
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

    _phase("callee_exit_mx_modes_initial")
    callee_exit_mx_modes = _build_callee_exit_mx_modes(callee_exit_mx)

    # Variant discovery originally runs before exit-M/X autoroute can be
    # applied to post-JSR/JSL fall-through decoding. Re-scan with the
    # learned exit map so call sites after a mutating callee contribute
    # their real runtime entry widths to downstream callees.
    exit_variant_total = 0
    for exit_variant_round in range(16):
        exit_variant_added = _discover_variants_from_current_entries(
            parsed, rom, variants, set(), dispatch_helpers,
            all_data_regions=all_data_regions or None,
            pruned_variants=set(),
            callee_exit_mx=callee_exit_mx,
            callee_exit_mx_modes=callee_exit_mx_modes)
        if not exit_variant_added:
            break
        exit_variant_total += exit_variant_added
        for _bank2, _cfg_path2, cfg2 in parsed:
            cfg2.exit_mx_at_per_variant.clear()
        exit_mx_fixes = autoroute_exit_mx(
            parsed, rom, dispatch_helpers=dispatch_helpers)
        callee_exit_mx, _cfg_exit_count, _decl_exit_count, \
            _per_variant_count = _rebuild_callee_exit_mx(parsed, variants)
        callee_exit_mx_modes = _build_callee_exit_mx_modes(callee_exit_mx)
        print(f"  exit-mx variant refresh {exit_variant_round + 1}: "
              f"added {exit_variant_added} entry variant(s); "
              f"refreshed {len(exit_mx_fixes)} exit-mx routes")
    else:
        print("  WARNING: exit-mx variant refresh hit 16 rounds")
    if exit_variant_total:
        print(f"  exit-mx variant refresh: added {exit_variant_total} "
              f"entry variant(s) total")

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

    def _autopromote_targets(parsed_repo, demands: set, *, source_kind: str) -> set:
        """Add BankEntry records for any (addr, m, x) demand tuple not
        already represented in the bank's cfg. Shared between Call-target
        and Goto-target auto-promotion. Returns the newly-added
        (addr24, m, x) entries.

        `source_kind` is "call" or "goto" — only used for logging context;
        promotion logic is identical (same bucket-and-merge shape).
        """
        if not demands:
            return 0
        bank_set = {b for (b, _p, _c) in parsed_repo}
        by_bank: dict[int, list[tuple[int, int, int]]] = {}
        for addr, em, ex in demands:
            tbank = (addr >> 16) & 0xFF
            tpc = addr & 0xFFFF
            # LoROM bank-mirror: banks $80-$BF are byte-identical to
            # $00-$3F. If the target bank has no cfg but its mirror
            # does, redirect the demand to the mirror's cfg so the
            # function's (m,x) variant gets emitted in the canonical
            # bank rather than dropping into the cross-ROM-bank stub
            # path. Long-mode vectors in Mega Man X (JML $00 -> $80)
            # are the canonical case this unblocks. (2026-05-21)
            if tbank not in bank_set:
                mirror = tbank ^ 0x80
                if (tbank < 0x40 or 0x80 <= tbank < 0xC0) and mirror in bank_set:
                    tbank = mirror
            # Refuse to synthesize a function entry inside a cfg
            # data_region. That range was declared as data; turning
            # bytes into a callable handler defeats the directive.
            if all_data_regions and any(
                    (b & 0xFF) == tbank and (s & 0xFFFF) <= tpc < (e & 0xFFFF)
                    for (b, s, e) in all_data_regions):
                continue
            by_bank.setdefault(tbank, []).append((tpc, em, ex))
        bank_index = {b: cfg for (b, _p, cfg) in parsed_repo}
        added_local: set = set()
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
                added_local.add(((bank << 16) | (pc & 0xFFFF), em, ex))
        return added_local

    # Bumped 2026-05-03: with the new callee-exit-(m,x) propagation,
    # decoder discovers more variants in transitive callees. 8 passes
    # leaves ~239 unresolved externals; 24 converges in practice.
    max_passes = 24
    last_unresolved: set = set()
    pending_variant_entries: set = set()
    exit_mx_rescan_all = False

    # A2: parallel emit Pool — None when --jobs <= 1 (sequential, in-
    # process call to _emit_bank_one). Pool persists across passes so
    # workers pay the Python-import startup cost once. Per-pass dynamic
    # state (name_map, callee_exit_mx, trampoline_returns) is passed in
    # each work item; workers do not rely on shared globals.
    pool = None
    if args.jobs > 1:
        import multiprocessing as _mp
        pool = _mp.Pool(processes=args.jobs)
        print(f"v2_regen: parallel emit enabled — {args.jobs} workers")

    # Cumulative drains across passes. Pre-A2 these lived in codegen
    # module globals; with workers each one has its own — we union
    # into these main-process accumulators after every pass and
    # reseed via set_trampoline_returns() for the next pass.
    cumulative_trampoline_returns: set = set()
    cumulative_rejected_calls: set = set()

    # Emit-truth variant prune state. `valid_variants_map` is the
    # per-target survivor set fed to codegen (empty on full-regeneration
    # pass 0 => emit all four). With --banks partial regen, seed it from
    # generated definitions for skipped banks so regenerated callers do
    # not reference variant symbols that the preserved sources do not
    # actually define.
    preserved_valid_variants = _scan_skipped_bank_valid_variants(
        out_dir, parsed, only_banks)
    if preserved_valid_variants:
        count = sum(len(v) for v in preserved_valid_variants.values())
        print(f"  --banks filter active; seeded {count} emitted variant "
              f"definition(s) from skipped bank sources")
    skipped_bank_results = _read_skipped_bank_results(out_dir, only_banks)
    skipped_ref_graph = _scan_variant_refs(skipped_bank_results, parsed)
    skipped_link_targets = _scan_link_variant_targets(
        skipped_bank_results, parsed)
    skipped_dirty_variants, skipped_emitted_variants = _scan_dirty_variants(
        skipped_bank_results, parsed)
    if skipped_bank_results:
        ref_count = sum(len(v) for v in skipped_ref_graph.values())
        print(f"  --banks filter active; scanned {len(skipped_bank_results)} "
              f"skipped bank source(s), {ref_count} direct ref(s), "
              f"{len(skipped_link_targets)} link target(s), "
              f"{len(skipped_dirty_variants)} dirty variant marker(s)")
    if only_banks is not None and skipped_link_targets:
        partial_link_demands = {
            v for v in skipped_link_targets
            if ((v[0] >> 16) & 0xFF) in only_banks
        }
        if partial_link_demands:
            promoted = _autopromote_targets(
                parsed, partial_link_demands, source_kind="partial-link")
            promoted = promoted or set()
            pending_variant_entries |= partial_link_demands | promoted
            print(f"  --banks filter active; seeded "
                  f"{len(partial_link_demands)} variant demand(s) from "
                  f"skipped source link refs "
                  f"({len(promoted)} promoted)")
    valid_variants_map: dict = dict(preserved_valid_variants)
    set_valid_variants(valid_variants_map)
    # `cumulative_dirty_variants` accumulates every variant whose body
    # emitted a stub marker; `cumulative_pruned` is the set actually
    # dropped (dirty non-canonical clones with a clean canonical sibling).
    # Both grow monotonically -> the prune converges alongside auto-promote.
    cumulative_dirty_variants: set = set()
    cumulative_emitted_variants: set = set()
    cumulative_pruned: set = set()
    # In-cfg-set banks: the reference-taint prune only treats a missing
    # callee variant as a dangling reference when its bank is in the cfg
    # set (out-of-set targets get loud stub bodies, never dangle).
    bank_set = {b for (b, _p, _c) in parsed}

    # ── Reference-taint non-convergence guard ───────────────────────────
    # Failure mode (observed regenerating ALttP, 2026-06-02): once
    # auto-promote + emit-truth prune stabilize, the reference-taint prune
    # can lock into peeling a tiny constant number of wrong-width caller
    # clones per pass — each full re-emit exposes one more dangling layer —
    # and never reaches `ref_prunable == 0`. It would otherwise run to the
    # wall-clock cap. A HEALTHY build does only a handful of reference-taint
    # passes (MMX: 0–2) and terminates. So: if the prune does nothing BUT
    # peel a small (<= RT_SMALL_DROP) count for RT_STALL_LIMIT consecutive
    # passes, declare non-convergence and fail early with a diagnostic.
    # Conservative — the limit sits far above any legitimate peel depth, so
    # a real (deep-but-finite) drain finishes well before it trips.
    RT_SMALL_DROP = 4
    RT_STALL_LIMIT = 12
    rt_small_streak = 0
    # Backstop independent of drop size: SMW's varying drops (e.g. an
    # occasional 9 amid 1s) reset rt_small_streak and slip past RT_STALL_LIMIT.
    # With the freeze-exit-mx fix a healthy build needs only 1-2 ref-taint
    # passes, so a total far above that (any churn at all) is non-convergence
    # — abort cleanly rather than running to the wall-clock cap. Backstop, not
    # the fix: the fix is freezing exit-(M,X) on ref-taint-only passes.
    RT_TOTAL_LIMIT = 40
    rt_total_passes = 0
    # Rolling window of the last N reference-taint passes' fixpoint inputs,
    # dumped to _refprune_snap_NNN.json. Lets a convergence fix be iterated
    # OFFLINE (replay _propagate_reference_taint/_compute_prunable per pass),
    # and — because it spans the run-up (e.g. 5,3,4,2,3) BEFORE the 1/pass
    # onset — lets the replay start a few passes back to catch a
    # pre-dependency that sets up the cycle.
    RT_SNAP_WINDOW = 8

    for pass_idx in range(max_passes):
        _phase(f"emit_pass_{pass_idx}")
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
        # Auto-promoted targets materialize after the initial
        # variant-discovery pass.  Decode any current entries that have
        # not yet fed discovery, then clone newly demanded variants for
        # known callees before this pass emits runtime dispatch switches.
        variant_added = 0
        if pending_variant_entries or exit_mx_rescan_all:
            seed_entries = None if exit_mx_rescan_all else pending_variant_entries
            scan_decoded = set() if exit_mx_rescan_all else decoded
            variant_added = _discover_variants_from_current_entries(
                parsed, rom, variants, scan_decoded, dispatch_helpers,
                all_data_regions=all_data_regions or None,
                pruned_variants=cumulative_pruned,
                pending_entries=seed_entries,
                callee_exit_mx=callee_exit_mx,
                callee_exit_mx_modes=callee_exit_mx_modes)
            pending_variant_entries = set()
            exit_mx_rescan_all = False
        if variant_added:
            if cumulative_pruned:
                valid_variants_map = _apply_variant_prune(
                    parsed, cumulative_pruned, preserved_valid_variants)
            print(f"  variant discovery refresh: added {variant_added} "
                  f"entry variant(s) from current cfg entries")
        # Build per-bank work items. Banks outside the --banks filter
        # are skipped here; the work item dict carries everything
        # _emit_bank_one needs (no shared globals across workers).
        work_items: list = []
        for bank, cfg_path, cfg in parsed:
            if only_banks is not None and bank not in only_banks:
                if pass_idx == 0:
                    print(f"  SKIP  bank ${bank:02X}: not in --banks filter (keeping existing .c)")
                continue
            ind_dispatch_map = None
            ind_list = getattr(cfg, 'indirect_dispatch', None) or []
            if ind_list:
                ind_dispatch_map = {}
                for d in ind_list:
                    pc24 = (bank << 16) | (d['site_pc16'] & 0xFFFF)
                    ind_dispatch_map[pc24] = d
            work_items.append({
                'bank': bank,
                'cfg': cfg,
                'rom': rom,
                'rom_size': len(rom),
                'dispatch_helpers': dispatch_helpers,
                'indirect_dispatch_map': ind_dispatch_map,
                'name_map': name_map,
                'force_variant_at': force_variant_map,
                'valid_variants': valid_variants_map,
                'trampoline_returns': cumulative_trampoline_returns,
                'callee_exit_mx': callee_exit_mx,
                'callee_exit_mx_modes': callee_exit_mx_modes,
                'inline_arg_map': inline_arg_map,
                'decode_cache_enabled': not args.no_decode_cache,
            })

        # Run emit. Pool path used when --jobs > 1 and there's more
        # than one bank to emit; jobs=1 path stays fully in-process
        # (no pickling, byte-identical output to the pre-A2 pipeline).
        if pool is not None and len(work_items) > 1:
            results = pool.map(_emit_bank_one, work_items)
        else:
            results = [_emit_bank_one(wi) for wi in work_items]

        # Merge worker outputs back into the main process's
        # per-pass + cumulative accumulators.
        pass_unresolved_calls: set = set()
        for r in results:
            if r.get('bank_field_warning'):
                print(r['bank_field_warning'])
            bank = r['bank']
            if r['status'] == 'fail':
                print(f"  FAIL  bank ${bank:02X}: {r['error']}")
                if r.get('traceback'):
                    print(r['traceback'])
                failed.append((bank, r['error']))
                continue
            out_path = out_dir / f'bank{bank:02x}_v2.c'
            write_if_changed(out_path, r['src'])
            all_suppressed.extend(r['suppressed'])
            all_const_z_folds.extend(r['const_z_folds'])
            all_dispatch_suppressed.extend(r['dispatch_suppressed'])
            all_unresolved_indirects.extend(r['unresolved_indirects'])
            pass_unresolved_calls.update(r['unresolved_calls'])
            cumulative_rejected_calls.update(r['rejected_call_targets'])
            cumulative_trampoline_returns.update(
                r['trampoline_returns_local'])
            if pass_idx == 0:
                print(f"  OK    bank ${bank:02X}: {r['cfg_entries_count']} entries -> {out_path}")
            succeeded += 1
        # Reseed main's _TRAMPOLINE_RETURNS for any later main-process
        # emit paths (none today) and keep main's view consistent.
        set_trampoline_returns(cumulative_trampoline_returns)

        # ── Emit-truth clean-sibling variant prune ──────────────────
        # A variant is 'dirty' iff its emitted body contains a stub
        # marker (the genuine lint failure). When a dirty variant is a
        # NON-canonical clone AND its entry has a clean canonical
        # sibling, the canonical proves the bytes are valid code at the
        # cfg-declared width — so the dirty clone is a wrong-width
        # decode (misaligned operands -> phantom branches / garbage
        # calls) that can never be validly reached. Drop it: remove the
        # entry (no body, no forward decl) and record the survivor set
        # so the runtime (m,x) dispatch switches stop referencing it.
        # Never prune a canonical (the alias binds to it). Bases whose
        # canonical is itself dirty, or with no clean sibling, are
        # left intact and surface in the stub lint as genuine residue.
        #
        # AUTO-PROMOTED targets (synthetic `bank_XX_YYYY`, demanded by a
        # JSR/JMP (abs,X) table the decoder auto-recovered) have NO cfg
        # entry, so canonical_variants is empty for them and the
        # canonical-sibling test below can never fire. They are the bulk
        # of the wrong-width residue — a phantom JSR (abs,X) inside a
        # wrong-width parent auto-recovers a garbage table and promotes
        # garbage entry PCs, each of which then misdecodes at its own
        # wrong widths. For these (canon == empty) prune a dirty variant
        # whenever ANY clean sibling variant was emitted (clean at SOME
        # width proves the bytes are real code there); the all-dirty
        # genuine RAM/computed-dispatch sites have no clean sibling and
        # correctly remain as loud trap residue.
        dirty_now, emitted_now = _scan_dirty_variants(results, parsed)
        cumulative_dirty_variants |= dirty_now
        cumulative_emitted_variants |= emitted_now
        # Variants directly referenced by a surviving body
        # are provably reached at the referenced width; protect them from
        # the wrong-width prune so a genuinely-reached variant whose body
        # carries a pending indirect-dispatch stub is never dropped (which
        # would dangle the caller's direct reference -> LNK2019).
        emit_ref_graph = _scan_variant_refs(results, parsed)
        protect_ref_graph = _merge_variant_ref_graphs(
            skipped_ref_graph, emit_ref_graph)
        protected_variants = _protected_ref_targets(
            protect_ref_graph, canonical_variants,
            cumulative_dirty_variants | skipped_dirty_variants)
        protected_variants |= skipped_link_targets
        prunable = _compute_prunable(
            cumulative_dirty_variants, cumulative_emitted_variants,
            canonical_variants, protected_variants)
        newly_pruned = prunable - cumulative_pruned
        if newly_pruned:
            _reject_partial_root_prune(
                newly_pruned, skipped_link_targets, "prune")
            cumulative_pruned |= newly_pruned
            valid_variants_map = _apply_variant_prune(
                parsed, cumulative_pruned, preserved_valid_variants)
            print(f"  emit-truth prune: dropped {len(newly_pruned)} "
                  f"wrong-width variant(s) this pass "
                  f"({len(cumulative_pruned)} total); re-emitting")

        # Call-target demands. Workers drain their own globals during
        # emit and return them; pass_unresolved_calls is the union of
        # those drains. Also union main's set in case the autoroute
        # pre-passes (which run in main) leaked any. Subtract pruned
        # variants so auto-promote can't resurrect a dropped body.
        unresolved_calls = (pass_unresolved_calls
                            | take_unresolved_call_targets()) - cumulative_pruned
        last_unresolved = unresolved_calls

        added_entries = (_autopromote_targets(
            parsed, unresolved_calls, source_kind="call")
            if unresolved_calls else set())
        pending_variant_entries |= added_entries
        added = len(added_entries)

        if added == 0 and not newly_pruned and not variant_added:
            # ── Reference-taint prune (convergence guard) ────────────
            # Emit-truth prune + auto-promote have stabilized. The
            # bf8a34b runtime-(m,x) policy still emits wrong-width CALLER
            # clones; at the wrong width a clone's DIRECT successor
            # (tail-call / resolved single call, emitted at the clone's
            # own inherited width) names a callee variant that was never
            # emitted at that width -> a dangling link reference
            # (LNK2019). Such a clone is itself wrong-width and
            # unreachable at runtime (the live (m,x) selects the
            # canonical variant). Taint it via the DIRECT reference graph
            # (4-way switch cases are excluded — they never dangle and
            # would cascade taint across the whole call graph) and prune
            # any whose canonical sibling is clean. The "currently
            # emitted" set is THIS pass's `emitted_now`, NOT cumulative:
            # a variant emitted in an early all-widths pass then dropped
            # from valid_variants (without being pruned) is no longer on
            # disk, so cumulative would mask the dangle. Computed only
            # HERE, at convergence, so "not emitted" is stable (no callee
            # is merely awaiting a later auto-promote pass). New prunes
            # re-emit and may expose a further dangling layer; the loop
            # iterates to a fixpoint (monotonic — cumulative_pruned only
            # grows).
            ref_graph = _scan_variant_refs(results, parsed)
            protect_ref_graph = _merge_variant_ref_graphs(
                skipped_ref_graph, ref_graph)
            ref_protected = _protected_ref_targets(
                protect_ref_graph, canonical_variants,
                cumulative_dirty_variants | skipped_dirty_variants)
            ref_protected |= skipped_link_targets
            emitted_for_ref = emitted_now | skipped_emitted_variants
            ref_prunable = set()
            while True:
                tainted = _propagate_reference_taint(
                    cumulative_dirty_variants, ref_graph,
                    emitted_for_ref, bank_set,
                    cumulative_pruned | ref_prunable)
                next_ref_prunable = (
                    _compute_prunable(
                        tainted, emitted_for_ref, canonical_variants,
                        ref_protected)
                    - cumulative_pruned
                    - ref_prunable)
                if not next_ref_prunable:
                    break
                ref_prunable |= next_ref_prunable
            if not ref_prunable:
                break
            _reject_partial_root_prune(
                ref_prunable, skipped_link_targets, "reference-prune")
            # ── Rolling fixpoint snapshot (offline-replay / fast test loop) ──
            # Dump THIS pass's exact prune inputs before applying the drop,
            # keeping only the last RT_SNAP_WINDOW. Each file is a resumable
            # replay point (re-run _propagate_reference_taint/_compute_prunable
            # against it); the window spans the pre-lock run-up so a fix can
            # start a few passes back.
            # Serialize by real type: variant ids are (addr, m, x) tuples
            # (round-trip as [addr,m,x] lists); canonical_variants and
            # bank_set are plain ints. _nm() is human-readable only and
            # guards against non-tuples.
            def _nm(v):
                if not isinstance(v, tuple):
                    return v
                a, mm, xx = v
                return f"bank_{(a >> 16) & 0xFF:02X}_{a & 0xFFFF:04X}_M{mm}X{xx}"

            def _t(v):
                return list(v) if isinstance(v, tuple) else v
            _dangling = {}
            for _v in ref_prunable:
                for _e in ref_graph.get(_v, ()):
                    if ((_e[0] >> 16) & 0xFF) in bank_set and _e not in emitted_now:
                        _dangling.setdefault(_nm(_v), []).append(_nm(_e))
            _snap = {
                'pass_idx': pass_idx,
                'dropped': sorted(_nm(v) for v in ref_prunable),
                'dangling_edges': _dangling,
                'ref_graph': [[_t(k), [_t(e) for e in vs]]
                              for k, vs in ref_graph.items()],
                'emitted_now': [_t(v) for v in emitted_now],
                'cumulative_pruned': [_t(v) for v in cumulative_pruned],
                'cumulative_dirty': [_t(v) for v in cumulative_dirty_variants],
                'canonical': sorted(canonical_variants),
                'bank_set': sorted(bank_set),
            }
            import json as _json, os as _os
            with open(f'_refprune_snap_{pass_idx:03d}.json', 'w') as _fh:
                _json.dump(_snap, _fh)
            if pass_idx - RT_SNAP_WINDOW >= 0:
                try:
                    _os.remove(f'_refprune_snap_{pass_idx - RT_SNAP_WINDOW:03d}.json')
                except OSError:
                    pass
            cumulative_pruned |= ref_prunable
            valid_variants_map = _apply_variant_prune(
                parsed, cumulative_pruned, preserved_valid_variants)
            print(f"  reference-taint prune: dropped {len(ref_prunable)} "
                  f"wrong-width caller clone(s) "
                  f"({len(cumulative_pruned)} total); re-emitting "
                  f"[snap pass {pass_idx}, dropped={_snap['dropped']}]")
            # Non-convergence guard: a healthy build resolves the dangling
            # layers in a few passes. A tiny constant drop sustained over
            # many passes is the ALttP-style lock (each re-emit re-exposes
            # one layer) and never reaches ref_prunable == 0 -> bail before
            # the wall-clock cap with an actionable diagnostic.
            rt_total_passes += 1
            if rt_total_passes >= RT_TOTAL_LIMIT:
                print(f"\n!!! v2_regen ABORT: reference-taint prune ran "
                      f"{rt_total_passes} passes without reaching a fixpoint "
                      f"({len(cumulative_pruned)} total pruned). Drop-size-"
                      f"independent backstop ({RT_TOTAL_LIMIT}) tripped — a "
                      f"healthy build converges in 1-2 ref-taint passes. Last "
                      f"{RT_SNAP_WINDOW} fixpoint snapshots: _refprune_snap_*."
                      f"json — replay the prune offline; fix the cycle in the "
                      f"recompiler, not here.",
                      file=sys.stderr, flush=True)
                sys.exit(3)
            if len(ref_prunable) <= RT_SMALL_DROP:
                rt_small_streak += 1
            else:
                rt_small_streak = 0
            if rt_small_streak >= RT_STALL_LIMIT:
                print(f"\n!!! v2_regen ABORT: reference-taint prune is NOT "
                      f"converging — dropped <= {RT_SMALL_DROP} wrong-width "
                      f"clone(s)/pass for {rt_small_streak} consecutive passes "
                      f"({len(cumulative_pruned)} total) without reaching a "
                      f"fixpoint. Non-terminating wrong-width-clone cycle (each "
                      f"full re-emit re-exposes one dangling layer). Last "
                      f"{RT_SNAP_WINDOW} fixpoint snapshots: _refprune_snap_*."
                      f"json — replay the prune offline to iterate a fix; fix "
                      f"the cycle in the recompiler, not here.",
                      file=sys.stderr, flush=True)
                sys.exit(3)
            # fall through -> exit-mx refresh -> loop re-emits
        else:
            # A pass that did real structural work (auto-promote add /
            # emit-truth prune / variant discovery) — reset the stall streak.
            rt_small_streak = 0
        # Auto-promoted callees did not exist when the earlier exit-M/X
        # autoroute ran. Refresh the auto-detected per-variant exit map
        # before the next emit pass so callers decode post-JSR/JSL code
        # with the newly-known return M/X state.
        for _bank2, _cfg_path2, cfg2 in parsed:
            cfg2.exit_mx_at_per_variant.clear()
        refreshed_exit_mx_fixes = autoroute_exit_mx(
            parsed, rom, dispatch_helpers=dispatch_helpers)
        callee_exit_mx = {}
        cfg_exit_mx_count = 0
        declared_exit_mx = {}
        for _bank2, _cfg_path2, cfg2 in parsed:
            for (b_id, addr16, m_val, x_val) in cfg2.exit_mx_at:
                declared_exit_mx[(b_id & 0xFF, addr16 & 0xFFFF)] = (
                    m_val, x_val)
        for (b_id, addr16), (ex_m, ex_xf) in declared_exit_mx.items():
            target_pc24 = (b_id << 16) | addr16
            mx_set = variants.get(target_pc24)
            if mx_set:
                for em, ex2 in mx_set:
                    callee_exit_mx[(target_pc24, em, ex2)] = (
                        ex_m, ex_xf)
                    cfg_exit_mx_count += 1
            else:
                callee_exit_mx[(target_pc24, 1, 1)] = (ex_m, ex_xf)
                cfg_exit_mx_count += 1
        per_variant_count = 0
        for _bank2, _cfg_path2, cfg2 in parsed:
            for (b_id, addr16, em_in, ex_in, ex_m, ex_xf) in \
                    cfg2.exit_mx_at_per_variant:
                target_pc24 = ((b_id & 0xFF) << 16) | (addr16 & 0xFFFF)
                callee_exit_mx[(target_pc24, em_in & 1, ex_in & 1)] = (
                    ex_m & 1, ex_xf & 1)
                per_variant_count += 1
        callee_exit_mx_modes = _build_callee_exit_mx_modes(callee_exit_mx)
        exit_mx_rescan_all = True
        print(
            f"  auto-promote pass {pass_idx + 1}: "
            f"added {added} entries "
            f"and {variant_added} refreshed variants "
            f"(calls={len(unresolved_calls)}); "
            f"refreshed {len(refreshed_exit_mx_fixes)} exit-mx routes; "
            f"re-emitting"
        )
        # Refresh the codegen name resolver with the newly-synthesized
        # entries (e.g. JSR/JSL targets that auto-promote turned into
        # `bank_BB_AAAA` BankEntries). emit_function's tail-call resolver
        # consults _NAME_RESOLVER when a jump past end: lands on a known
        # entry; without this refresh, JMP-targets that the variant
        # discovery raised as siblings AFTER the initial name_map was
        # built would fall through to the unresolved-goto trap instead
        # of emitting a tail-call. (Zelda intro fix follow-up 2026-05-17:
        # the decoder's sibling-entry rejection turns JMPs into dangling
        # successors that emit_function MUST resolve to a name.)
        for bank2, _cfg_path2, cfg2 in parsed:
            for entry in cfg2.entries:
                if entry.name:
                    name_map[(bank2 << 16) | (entry.start & 0xFFFF)] = entry.name
        set_name_resolver(name_map)

    # A2: parallel emit complete. Close workers; the remaining stub-
    # file + dispatch-table emit run sequentially in main.
    if pool is not None:
        pool.close()
        pool.join()
        pool = None

    # Final pass: any still-unresolved Call targets after the last emit
    # belong to ROM banks not in the cfg set (e.g. data decoded as code
    # that produced a JSL into bank $24/$67/etc.). Emit one shared stub
    # file with empty bodies so the linker is happy. Real execution
    # paths shouldn't reach these; if they do, the stubs are no-ops.
    by_bank: dict[int, set] = {}
    bank_set = {b for (b, _p, _c) in parsed}
    # Build a quick lookup of every (bank, pc) pair that the cfg set
    # declares, so we can short-circuit cross-bank demands that resolve
    # via LoROM bank-mirror to a known function entry.
    cfg_entry_pcs: dict[int, set] = {}
    for (b, _p, c) in parsed:
        cfg_entry_pcs.setdefault(b, {e.start & 0xFFFF for e in c.entries})
    for addr, em, ex in last_unresolved:
        bank = (addr >> 16) & 0xFF
        if bank in bank_set:
            continue
        # LoROM mirror: if $80-$BF demand falls through here but $00-$3F
        # has a cfg with the same PC declared, the call site already
        # resolved via codegen's name-resolver mirror — no stub needed.
        # Same for $00-$3F -> $80-$BF (the other direction; rare).
        mirror = bank ^ 0x80
        if (bank < 0x40 or 0x80 <= bank < 0xC0) and mirror in bank_set:
            mirror_pcs = cfg_entry_pcs.get(mirror, set())
            if (addr & 0xFFFF) in mirror_pcs:
                continue
        by_bank.setdefault(bank, set()).add((addr & 0xFFFF, em & 1, ex & 1))
    # Always emit the stub file (even when no stubs are needed) so the
    # vcxproj/CMake/Makefile can list it as a fixed compile unit
    # unconditionally. An empty stub file compiles to an empty TU. The
    # build system would otherwise need a conditional include for a
    # sometimes-present file, and missing-source errors after a clean
    # regen are confusing. (2026-05-23: runtime (m, x) dispatch raised
    # cross-bank variant demand 4× in MMX, exposing this path.)
    #
    # With --banks filter active: the demand set collected here only
    # reflects the filtered banks' emit, not the full ROM. The existing
    # stub file from a previous full regen still covers the demands
    # from skipped banks. Preserve it.
    stub_path = out_dir / 'unresolved_stubs_v2.c'
    if only_banks is not None:
        if stub_path.exists():
            print(f"  --banks filter active; preserving existing {stub_path}")
        else:
            print(f"  --banks filter active; no existing {stub_path} to preserve "
                  f"(run a full regen first)")
        # Skip the stub rewrite entirely, but keep dispatch_v2.c in sync
        # with the generated sources that are actually on disk.
        _emit_dispatch_table(out_dir, parsed, cumulative_pruned, inline_arg_map)
        if failed:
            return 1
        output_workspace.publish()
        print(f"  atomically published generated output -> "
              f"{output_workspace.target}")
        return 0
    lines = [
        '/* Auto-generated by snesrecomp v2 v2_regen. Do NOT hand-edit.',
        ' *',
        ' * Stub bodies for Call targets that resolved to a ROM bank not',
        ' * in the cfg set. These are typically data decoded as code',
        ' * (garbled JSL operands). Real execution paths should never',
        ' * reach them. By default each stub records the trap and returns a',
        ' * stack-safe no-op; with --tier-down-stubs it instead runs the real',
        ' * bytes via the interpreter tier (interp_tier_dispatch_bank_miss)',
        ' * and records to the Tier-2 gap manifest. Either way cpu->S stays',
        ' * balanced (pops the paired caller frame per hrv).',
        ' * One stub per (target, m, x) variant requested by the gen.',
        ' *',
        ' * Always emitted — file may be empty (no stubs needed) when',
        ' * every (target, m, x) demand resolved within the cfg set.',
        ' *',
        ' * Each body mirrors the emitted-function prologue (entry-S /',
        ' * host-return-validity capture incl. tailcall-context take) and',
        ' * exits via cpu_unresolved_abandon_balanced so a runtime fire',
        ' * leaves cpu->S balanced (pops the paired caller frame per hrv)',
        ' * instead of orphaning it — see the 2026-06-09 orphaned-frame',
        ' * leak class (cpu_state.h host_return_valid).',
        ' */',
        '',
        '#include "cpu_state.h"',
        '#include "cpu_trace.h"',
        '#include "common_cpu_infra.h"',
        '',
    ]
    total_stubs = 0
    # Effective bank-miss tier-down opt-in: the --tier-down-stubs CLI flag OR
    # a `tier_down_stubs` directive in any cfg (game-wide, persists across
    # regens without re-passing the flag). See docs/MULTI_TIER.md Phase 4.
    tier_down_stubs_on = args.tier_down_stubs or any(
        getattr(c, 'tier_down_stubs', False) for _b, _p, c in parsed)
    if tier_down_stubs_on:
        src = 'cfg directive' if not args.tier_down_stubs else '--tier-down-stubs'
        print(f"  tier_down_stubs ON ({src}): bank-miss stubs run via the "
              f"interpreter tier")
    for bank in sorted(by_bank):
        for pc, em, ex in sorted(by_bank[bank]):
            name = f'bank_{bank:02X}_{pc:04X}_M{em}X{ex}'
            target_pc24 = (bank << 16) | (pc & 0xFFFF)
            lines.append(f'RecompReturn {name}(CpuState *cpu) {{')
            lines.append('  uint16 _entry_s = cpu->S;')
            lines.append('  uint8 _hrv = cpu->host_return_valid;')
            lines.append('  if (cpu_take_tailcall_return_context(&_entry_s, &_hrv)) {')
            lines.append('    cpu->host_return_valid = _hrv;')
            lines.append('  }')
            if tier_down_stubs_on:
                # Phase-4 opt-in: run the real bytes via the interpreter tier
                # instead of the no-op trap. Bail -> same abandon (never worse);
                # every fire lands in the Tier-2 gap manifest as kind=bank_miss.
                lines.append(
                    f'  return interp_tier_dispatch_bank_miss(cpu, '
                    f'0x{target_pc24:06x}u, _entry_s, _hrv);')
            else:
                lines.append(
                    f'  (void)cpu_trace_unresolved_stub_trap(cpu, 0x{target_pc24:06x}, "{name}");')
                lines.append(
                    f'  return cpu_unresolved_abandon_balanced(cpu, '
                    f'0x{target_pc24:06x}u, _entry_s, _hrv);')
            lines.append('}')
            total_stubs += 1
    write_if_changed(stub_path, '\n'.join(lines) + '\n')
    if total_stubs:
        print(f"  emitted stubs for {total_stubs} cross-ROM-bank (target, m, x) variants -> {stub_path}")
    else:
        print(f"  no cross-ROM-bank stubs needed; emitted empty {stub_path}")

    # ── PEI-trampoline dispatch table emit (2026-05-24, narrow) ──────
    #
    # Runtime cpu_dispatch_pc (cpu_state.c) binary-searches this table
    # when an RTS/RTL emit hits the trampoline branch (cpu->S !=
    # _entry_s at a trampoline-flagged Return). Every emitted function
    # entry contributes a row keyed by pc24 with up to 4 fnptrs
    # (one per (m,x) variant; NULL when that variant wasn't emitted).
    name_for_pc24: dict[int, str] = {}
    name_to_pc24: dict[str, int] = {}
    for bank2, _cfg_path2, cfg2 in parsed:
        for entry in cfg2.entries:
            if entry.name:
                pc24 = (bank2 << 16) | (entry.start & 0xFFFF)
                name_for_pc24[pc24] = entry.name
                name_to_pc24.setdefault(entry.name, pc24)
    def _disp_name(pc24: int) -> str:
        n = name_for_pc24.get(pc24)
        if n is not None:
            return n
        bank = (pc24 >> 16) & 0xFF
        pc = pc24 & 0xFFFF
        return f"bank_{bank:02X}_{pc:04X}"
    disp_variants = _scan_generated_variant_defs(out_dir, name_to_pc24)
    # Never reference a PRUNED variant from the dispatch table. The
    # table is built from generated definitions above, so this is mostly
    # a safety net for stale partial-regeneration inputs.
    for (addr, em, ex) in cumulative_pruned:
        s = disp_variants.get(addr)
        if s is not None:
            s.discard((em & 1, ex & 1))
    sorted_pc24s = sorted(pc24 for pc24, mxs in disp_variants.items() if mxs)
    disp_path = out_dir / 'dispatch_v2.c'
    disp_lines = [
        '/* Auto-generated by snesrecomp v2 v2_regen. Do NOT hand-edit.',
        ' *',
        ' * PEI-trampoline dispatch table — runtime cpu_dispatch_pc() looks',
        ' * up function entries here when an RTS/RTL on a trampoline-flagged',
        ' * function hits the unbalanced-cpu->S branch in _emit_return.',
        ' *',
        ' * Sorted by pc24 for binary search. variant[] holds fnptrs for',
        ' * (M0X0, M0X1, M1X0, M1X1) — NULL when that variant wasn\'t emitted.',
        ' */',
        '',
        '#include "cpu_state.h"',
        '',
    ]
    fwd_seen: set[str] = set()
    for pc24 in sorted_pc24s:
        base = _disp_name(pc24)
        for em in (0, 1):
            for ex in (0, 1):
                if (em, ex) in disp_variants[pc24]:
                    name = f"{base}_M{em}X{ex}"
                    if name in fwd_seen:
                        continue
                    fwd_seen.add(name)
                    disp_lines.append(
                        f"RecompReturn {name}(CpuState *cpu);")
    disp_lines.append('')
    disp_lines.append('const DispatchEntry g_dispatch_table[] = {')
    if not sorted_pc24s:
        disp_lines.append(
            "    { 0xFFFFFFu, { NULL, NULL, NULL, NULL } },  /* sentinel — empty cfg */"
        )
    for pc24 in sorted_pc24s:
        base = _disp_name(pc24)
        inline_n = inline_arg_map.get(
            pc24, inline_arg_map.get(pc24 ^ 0x800000, 0))
        slots = ['NULL', 'NULL', 'NULL', 'NULL']
        for em, ex in disp_variants[pc24]:
            idx = ((em & 1) << 1) | (ex & 1)
            slots[idx] = f"{base}_M{em & 1}X{ex & 1}"
        disp_lines.append(
            f"    {{ 0x{pc24:06X}u, {{ {', '.join(slots)} }}, {inline_n} }},  "
            f"/* {base} */"
        )
    disp_lines.append('};')
    disp_lines.append('')
    disp_lines.append(
        f"const unsigned g_dispatch_table_count = "
        f"(unsigned)(sizeof(g_dispatch_table) / sizeof(g_dispatch_table[0]));"
    )
    disp_lines.append('')
    write_if_changed(disp_path, '\n'.join(disp_lines) + '\n')
    print(f"  emitted dispatch table with {len(sorted_pc24s)} entries -> {disp_path}")

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

    rejected = cumulative_rejected_calls | take_rejected_call_targets()
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

    final_elapsed = time.time() - regen_start_time
    cs = decode_cache_stats()
    if cs["enabled"]:
        total_lookups = cs["hits"] + cs["misses"]
        hit_pct = (100.0 * cs["hits"] / total_lookups) if total_lookups else 0.0
        print(f"\nv2_regen wall-clock: {final_elapsed:.1f}s; "
              f"decode_function cache (last phase): {cs['hits']}h/"
              f"{cs['misses']}m ({hit_pct:.1f}% hit rate); "
              f"{cs['size']} unique keys at end")
    else:
        print(f"\nv2_regen wall-clock: {final_elapsed:.1f}s; "
              f"decode cache: DISABLED (--no-decode-cache)")

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
    output_workspace.publish()
    print(f"  atomically published generated output -> "
          f"{output_workspace.target}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
