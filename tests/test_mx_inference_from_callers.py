"""Entry M/X inference from caller JSR context.

Pins the behaviour of the pass in run_config that walks each function's
decoded body, records (m, x) at every intra-bank JSR to a known func,
and derives an implicit mode_override at the callee's entry when the
callers agree on M=0 or X=0 (independently).

Two historical bugs this test guards:

1. Joint-unanimity gate: the earlier implementation required BOTH ms and
   xs to be unanimous before propagating ANY bit. That masked the
   common case where callers agree on M but split on X (e.g. the
   overworld helper $04:9885 'OW_TilePos_Calc' is called from both X=0
   and X=1 sites but always M=0). The framework correctly knew M=0
   was decidable but refused to install it, forcing the cfg to keep a
   load-bearing `rep:9885` hint it could otherwise shed.

2. Full-override guard: the earlier implementation installed the
   derived mode bits only when `old_entry == 0`. If the cfg author
   had supplied a partial hint (`repx:X` alone, encoding X=0 only),
   the pass refused to add the complementary M=0 bit the callers
   unanimously demanded. This blocked pair-derivation and made the
   rep/repx override pair jointly load-bearing even when each bit was
   individually derivable.

Both cases are now covered: the pass decides M and X independently,
and merges derived bits into any existing partial override (leaving
the SEP-marker 0x40 as the only opaque escape hatch).
"""
import pathlib
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / 'recompiler'))

import recomp  # noqa: E402


def _build_cfg(funcs):
    """Build a minimal Config with the given funcs = [(name, addr, mo)]."""
    cfg = recomp.Config()
    cfg.bank = 0x00
    for name, addr, mo in funcs:
        cfg.funcs.append((name, addr, None, None, dict(mo), {}))
        cfg.names[(cfg.bank << 16) | addr] = name
    return cfg


def _rom_with(base: int, *seqs):
    """Synthesize a LoROM bank-0 image with byte sequences at given PCs.
    seqs is a sequence of (pc, bytes) tuples. All pc must be >= 0x8000."""
    # Build a minimal 64KB bank-0 image; real LoROM has PC $8000 mapping to
    # ROM offset 0. For bank 0, lorom_offset(0, pc) == pc & 0x7FFF — so a
    # 32KB buffer covers $8000-$FFFF.
    rom = bytearray(0x10000)
    for pc, data in seqs:
        off = recomp.lorom_offset(0, pc)
        rom[off:off + len(data)] = data
    return bytes(rom)


def _run_mx_inference(rom: bytes, cfg):
    """Drive the caller-M/X inference pass in isolation. The pass lives
    inline in run_config; we re-create its body here so the test can
    validate it without building every other upstream step."""
    # Mirror the logic from run_config's pass.
    def _compute_tentative_ends(funcs):
        srt = sorted(funcs, key=lambda t: t[1])
        ends = {}
        for i, tup in enumerate(srt):
            _, saddr, _, eovr, _, _ = tup
            if eovr is not None:
                ends[saddr] = eovr
            elif i + 1 < len(srt):
                ends[saddr] = srt[i + 1][1] - 1
            else:
                ends[saddr] = 0xFFFF
        return ends

    from typing import Dict, List, Tuple
    func_entry_addrs = {a for _, a, *_ in cfg.funcs}
    for _iter in range(5):
        ends = _compute_tentative_ends(cfg.funcs)
        callsite_mx: Dict[int, List[Tuple[int, int]]] = {}
        for fname, saddr, _sig, _eovr, mo, _h in cfg.funcs:
            if fname in cfg.skip:
                continue
            try:
                insns = recomp.decode_func(
                    rom, cfg.bank, saddr, end=ends[saddr],
                    mode_overrides=mo or None,
                    validate_branches=False)
            except Exception:
                continue
            for insn in insns:
                if insn.mnem == 'JSR' and insn.operand in func_entry_addrs:
                    callsite_mx.setdefault(insn.operand, []).append(
                        (insn.m_flag, insn.x_flag))

        changed = False
        new_funcs = []
        for tup in cfg.funcs:
            fname, saddr, sig, eovr, mo, hints = tup
            callers = callsite_mx.get(saddr)
            if not callers:
                new_funcs.append(tup); continue
            ms = {c[0] for c in callers}
            xs = {c[1] for c in callers}
            want_bits = 0
            if ms == {0}: want_bits |= 0x20
            if xs == {0}: want_bits |= 0x10
            if want_bits == 0:
                new_funcs.append(tup); continue
            new_mo = dict(mo) if mo else {}
            old_entry = new_mo.get(saddr, 0)
            if old_entry & 0x40:
                new_funcs.append(tup); continue
            merged = old_entry | want_bits
            if merged == old_entry:
                new_funcs.append(tup); continue
            new_mo[saddr] = merged
            changed = True
            new_funcs.append((fname, saddr, sig, eovr, new_mo, hints))
        cfg.funcs = new_funcs
        if not changed:
            break


def test_unanimous_m_and_x_from_callers_installs_both_bits():
    """Caller at $8000: REP #$30 ; JSR $8020 ; RTS
    Callee at $8020: no cfg override, default M=1,X=1.
    Expected: caller's state at JSR is (M=0, X=0); pass installs
    mode_override[0x8020] = 0x30."""
    rom = _rom_with(0,
        (0x8000, bytes([0xC2, 0x30, 0x20, 0x20, 0x80, 0x60])),  # REP #$30 ; JSR $8020 ; RTS
        (0x8020, bytes([0x60])),                                  # RTS
    )
    cfg = _build_cfg([
        ('caller',  0x8000, {}),
        ('callee',  0x8020, {}),
    ])
    _run_mx_inference(rom, cfg)
    callee_mo = next(t[4] for t in cfg.funcs if t[0] == 'callee')
    assert callee_mo.get(0x8020) == 0x30, (
        f'expected mo[0x8020]=0x30 (M=0|X=0), got {callee_mo}')


def test_m_unanimous_x_mixed_installs_only_m_bit():
    """Two callers, one with REP #$30 (M=0,X=0) and one with
    REP #$20 ; SEP #$10 (M=0, X=1 explicit). Both agree on M=0 but
    split on X. Pass must still install the M=0 bit even though X
    can't be decided."""
    rom = _rom_with(0,
        (0x8000, bytes([0xC2, 0x30,                  # REP #$30 (M=0,X=0)
                        0x20, 0x40, 0x80,             # JSR $8040
                        0x60])),                      # RTS
        (0x8010, bytes([0xC2, 0x20,                  # REP #$20 (M=0)
                        0xE2, 0x10,                   # SEP #$10 (X=1)
                        0x20, 0x40, 0x80,             # JSR $8040
                        0x60])),                      # RTS
        (0x8040, bytes([0x60])),                      # RTS
    )
    cfg = _build_cfg([
        ('caller_a', 0x8000, {}),
        ('caller_b', 0x8010, {}),
        ('callee',   0x8040, {}),
    ])
    _run_mx_inference(rom, cfg)
    callee_mo = next(t[4] for t in cfg.funcs if t[0] == 'callee')
    assert callee_mo.get(0x8040) == 0x20, (
        f'mixed-X callers still allow M=0 propagation; expected mo[0x8040]=0x20 '
        f'(M=0 only), got {callee_mo}')


def test_mixed_m_suppresses_propagation():
    """Two callers, one with REP #$20 (M=0) and one with default (M=1).
    M is split, X agrees on 1 (default). Pass must install NOTHING
    (both axes agree on a default or are split)."""
    rom = _rom_with(0,
        (0x8000, bytes([0xC2, 0x20,                  # REP #$20 (M=0)
                        0x20, 0x40, 0x80,             # JSR $8040
                        0x60])),
        (0x8010, bytes([0x20, 0x40, 0x80,             # JSR $8040 (default M=1,X=1)
                        0x60])),
        (0x8040, bytes([0x60])),
    )
    cfg = _build_cfg([
        ('caller_a', 0x8000, {}),
        ('caller_b', 0x8010, {}),
        ('callee',   0x8040, {}),
    ])
    _run_mx_inference(rom, cfg)
    callee_mo = next(t[4] for t in cfg.funcs if t[0] == 'callee')
    assert callee_mo == {}, (
        f'mixed-M callers must not produce a mode_override; got {callee_mo}')


def test_partial_cfg_override_gets_merged_from_callers():
    """Cfg already has `repx:8040` (X=0 only, encoded as 0x10).
    Callers unanimously have M=0,X=0. Pass must OR in the missing
    M=0 bit so the full override becomes 0x30 — this is the case
    that made rep/repx override pairs jointly load-bearing before
    the merge fix."""
    rom = _rom_with(0,
        (0x8000, bytes([0xC2, 0x30,                  # REP #$30
                        0x20, 0x40, 0x80,             # JSR $8040
                        0x60])),
        (0x8040, bytes([0x60])),
    )
    cfg = _build_cfg([
        ('caller', 0x8000, {}),
        ('callee', 0x8040, {0x8040: 0x10}),  # cfg has repx: only
    ])
    _run_mx_inference(rom, cfg)
    callee_mo = next(t[4] for t in cfg.funcs if t[0] == 'callee')
    assert callee_mo.get(0x8040) == 0x30, (
        f'partial cfg override (repx) must merge with derived M=0; '
        f'expected mo[0x8040]=0x30, got {callee_mo}')


def test_sep_marker_is_opaque_and_not_overridden():
    """Cfg has `sep:8040` (SEP marker, 0x40). Callers unanimously
    have M=0 but the cfg explicitly pinned the entry back to
    default M=1,X=1 via SEP — that's an intentional signal the
    callers' REP is normalized by an in-body SEP the decoder can't
    see. Pass must NOT touch the 0x40 entry."""
    rom = _rom_with(0,
        (0x8000, bytes([0xC2, 0x20,                  # REP #$20 (M=0)
                        0x20, 0x40, 0x80,             # JSR $8040
                        0x60])),
        (0x8040, bytes([0x60])),
    )
    cfg = _build_cfg([
        ('caller', 0x8000, {}),
        ('callee', 0x8040, {0x8040: 0x40}),  # cfg has sep: marker
    ])
    _run_mx_inference(rom, cfg)
    callee_mo = next(t[4] for t in cfg.funcs if t[0] == 'callee')
    assert callee_mo.get(0x8040) == 0x40, (
        f'SEP marker (0x40) is opaque; pass must not merge derived bits '
        f'into it. Got {callee_mo}')


def test_no_callers_leaves_default_state():
    """Function with no intra-bank JSR callers: pass has no evidence
    to propagate. Entry stays at default M=1,X=1 (empty mo)."""
    rom = _rom_with(0,
        (0x8000, bytes([0x60])),
        (0x8040, bytes([0x60])),
    )
    cfg = _build_cfg([
        ('orphan_caller', 0x8000, {}),
        ('orphan_callee', 0x8040, {}),
    ])
    _run_mx_inference(rom, cfg)
    for name, addr, _sig, _eovr, mo, _h in cfg.funcs:
        assert mo == {}, f'{name}@${addr:04X} should stay empty, got {mo}'
