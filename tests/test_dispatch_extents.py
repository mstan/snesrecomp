"""Tier-1 structural cross-check: dispatch-table extents must match
SMWDisX.

For every JSL/JML site the recompiler classifies as a dispatch helper
call (ExecutePtr-style — see `_classify_dispatch_helper`), assert that
the count of inline-table entries the recompiler emits matches the
contiguous `dw <label>` directives at the same address in
SMWDisX/bank_XX.asm.

This is the structural counterpart to the framework's behavioral tests
and Phase B fuzz. Behavioral tests answer "did this run produce the
right output"; this test answers "did the recomp emit ALL the data the
ROM has." It catches dispatch-table truncations like SprStatus08 (28
emitted vs 0xC9 in ROM) and HandleSprite's kDispatch_8137 (6 vs 13)
the moment they regress, regardless of whether any current attract-
demo path exercises them.

Skipped when the test is run against a checkout that lacks SMWDisX
assets, smw.sfc, or the smwdisx_compare harness.
"""
import importlib.util
import pathlib
import sys


def _load_harness():
    # Use .absolute() rather than .resolve(): when the framework lives
    # under a Windows junction or symlink (snesrecomp/ inside a parent
    # game checkout), .resolve() follows it and breaks `.parent.parent`
    # traversal back to the parent game repo. .absolute() preserves the
    # surface path we were invoked under.
    framework_root = pathlib.Path(__file__).absolute().parent.parent
    game_root = framework_root.parent
    script = game_root / 'tools' / 'smwdisx_compare.py'
    sym_file = game_root / 'SMWDisX' / 'SMW_U.sym'
    rom = game_root / 'smw.sfc'
    if not (script.exists() and sym_file.exists() and rom.exists()):
        return None, None
    sys.path.insert(0, str(framework_root / 'recompiler'))
    spec = importlib.util.spec_from_file_location(
        'smwdisx_compare', str(script))
    mod = importlib.util.module_from_spec(spec)
    sys.modules['smwdisx_compare'] = mod
    spec.loader.exec_module(mod)
    return mod, rom


_BANKS = ['00', '01', '02', '03', '04', '05', '07', '0c', '0d']


def _collect_recomp_dispatches(rom: bytes, cfg, recomp):
    """Run decode_func over every cfg func and return
    {full_addr_of_jsl: entry_count} for every emitted dispatch."""
    out = {}
    sorted_funcs = sorted(cfg.funcs, key=lambda t: t[1])
    next_addr = {}
    for i, t in enumerate(sorted_funcs):
        next_addr[t[1]] = (sorted_funcs[i + 1][1]
                           if i + 1 < len(sorted_funcs) else 0x10000)

    known_func_starts = {(cfg.bank << 16) | a for _, a, *_ in cfg.funcs}
    known_addrs = set(known_func_starts)

    for fname, start, _sig, eovr, mo, _h in sorted_funcs:
        if fname in cfg.skip:
            continue
        end = eovr if eovr is not None else next_addr.get(start, 0x10000)
        try:
            insns = recomp.decode_func(
                rom, cfg.bank, start, end=end,
                mode_overrides=mo or None,
                jsl_dispatch=cfg.jsl_dispatch or None,
                jsl_dispatch_long=cfg.jsl_dispatch_long or None,
                exclude_ranges=cfg.exclude_ranges or None,
                dispatch_known_addrs=known_addrs,
                known_func_starts=known_func_starts,
                validate_branches=False,
            )
        except TypeError:
            # Older signature without one of the kwargs — fall back.
            insns = recomp.decode_func(
                rom, cfg.bank, start, end=end,
                mode_overrides=mo or None,
                jsl_dispatch=cfg.jsl_dispatch or None,
                jsl_dispatch_long=cfg.jsl_dispatch_long or None,
                validate_branches=False,
            )
        except Exception:
            continue
        for insn in insns:
            if not insn.dispatch_entries:
                continue
            full = (cfg.bank << 16) | (insn.addr & 0xFFFF)
            # If the same site is decoded under multiple function walks
            # (auto-promoted entry inside another body), keep the largest
            # count we observed — that's what the recomp would actually
            # emit at that address.
            prev = out.get(full, 0)
            out[full] = max(prev, len(insn.dispatch_entries))
    return out


def test_dispatch_extents_match_smwdisx():
    mod, rom_path = _load_harness()
    if mod is None:
        return
    rom = rom_path.read_bytes()
    labels = mod.load_symbols()
    addr_to_label = {l.addr: l.name for l in labels}
    cfgs = mod.load_cfgs(rom)

    import recomp  # noqa: E402

    failures = []
    for bank_hex in _BANKS:
        cfg = cfgs.get(bank_hex)
        if cfg is None:
            continue
        helper_addrs = (cfg.jsl_dispatch or set()) | (cfg.jsl_dispatch_long or set())
        helper_labels = {addr_to_label[a]
                         for a in helper_addrs
                         if a in addr_to_label}
        if not helper_labels:
            continue

        asm_dispatches = mod.parse_bank_dispatches(bank_hex, helper_labels)
        recomp_dispatches = _collect_recomp_dispatches(rom, cfg, recomp)

        # asm count of 0 with recomp count > 0 is a strong signal too:
        # it means the recompiler classified the JSL as dispatch and
        # walked entries past it, but SMWDisX has neither `dw` nor `dl`
        # at that address — either the ROM stores the table raw (`db`),
        # or the JSL isn't a dispatch at all and the recompiler's
        # auto-classification is wrong.
        common = set(asm_dispatches) & set(recomp_dispatches)
        for full_addr in sorted(common):
            asm_count = asm_dispatches[full_addr]
            recomp_count = recomp_dispatches[full_addr]
            if asm_count != recomp_count:
                bank = full_addr >> 16
                addr = full_addr & 0xFFFF
                failures.append(
                    f'bank {bank:02X}:{addr:04X} '
                    f'recomp emitted {recomp_count} entries, '
                    f'SMWDisX has {asm_count}'
                )

    if failures:
        msg = (
            f'Dispatch-table extent mismatch vs SMWDisX '
            f'({len(failures)} sites):\n  '
            + '\n  '.join(failures)
        )
        assert False, msg
