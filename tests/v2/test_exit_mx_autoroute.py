"""Pin v2 exit_mx_autoroute behaviour (leaf-only auto-detect).

The cfg `exit_mx_at <addr> <m> <x>` directive declares that the callee
at <addr> exits with (M, X) = (m, x). When set, the v2 decoder uses
the declared values for the JSR/JSL fall-through edge so the caller
resumes decoding with the right operand widths. Without it, the
decoder assumes M/X are preserved — wrong whenever the callee runs
an internal SEP/REP that never restores before return.

`v2.exit_mx_autoroute.detect_and_route` is a leaf-only auto-detector:
for every cfg `func` entry F whose decoded body contains NO JSR/JSL,
it analyses the (M, X) state at every RTS/RTL terminator. If those
unanimously differ from the entry state, it appends an
`exit_mx_at` tuple to F's owning BankCfg so the existing builder
picks it up.

Leaf-only restriction is mandatory: an earlier (2026-05-03) attempt at
unrestricted fixpoint regressed GraphicsDecompress into an infinite
loop and was reverted to opt-in.

Tests pin: positive (canonical SEP/RTS), negative (JSR-inside,
ambiguous, no change, no terminator, already-declared, unnamed).
"""
from _helpers import make_lorom_bank0  # noqa: E402

from dataclasses import dataclass, field
from v2.exit_mx_autoroute import detect_and_route  # noqa: E402


@dataclass
class _BankEntry:
    name: str
    start: int
    end: int = None
    entry_m: int = 1
    entry_x: int = 1
    tail_call_pc16: int = None


@dataclass
class _BankCfg:
    bank: int
    entries: list = field(default_factory=list)
    exit_mx_at: list = field(default_factory=list)


def test_detects_sep_then_rts_sets_m_to_1():
    """Canonical leaf: REP #$20 (enter M=0) then RTS. Function entry is
    declared M=1 so the REP changes A to 16-bit; exit (M, X) = (0, 1)."""
    # $8000: REP #$20  (C2 20)         — clear M flag
    # $8002: RTS       (60)
    rom = make_lorom_bank0({
        0x8000: bytes([0xC2, 0x20, 0x60]),
    })
    F = _BankEntry(name='F', start=0x8000)
    cfg = _BankCfg(bank=0x00, entries=[F])
    parsed = [(0x00, 'bank00.cfg', cfg)]

    fixes = detect_and_route(parsed, rom)
    assert len(fixes) == 1
    fx = fixes[0]
    assert fx.bank == 0x00
    assert fx.addr16 == 0x8000
    assert fx.fn_name == 'F'
    assert fx.entry_m == 1 and fx.entry_x == 1
    assert fx.exit_m == 0 and fx.exit_x == 1
    # cfg.exit_mx_at was mutated in place.
    assert cfg.exit_mx_at == [(0x00, 0x8000, 0, 1)]


def test_detects_rep_30_sets_both_to_0():
    """REP #$30 clears both M and X — exit (0, 0) from entry (1, 1)."""
    # $8000: REP #$30  (C2 30)
    # $8002: RTS       (60)
    rom = make_lorom_bank0({
        0x8000: bytes([0xC2, 0x30, 0x60]),
    })
    F = _BankEntry(name='F', start=0x8000)
    cfg = _BankCfg(bank=0x00, entries=[F])
    parsed = [(0x00, 'bank00.cfg', cfg)]

    fixes = detect_and_route(parsed, rom)
    assert len(fixes) == 1
    assert fixes[0].exit_m == 0 and fixes[0].exit_x == 0


def test_skips_when_body_has_jsr():
    """Non-leaf: contains JSR. Auto-router must skip (callee dependency
    is exactly what the prior fixpoint attempt regressed on)."""
    # $8000: REP #$20      (C2 20)
    # $8002: JSR $8100     (20 00 81)
    # $8005: RTS           (60)
    # $8100: RTS           (60)
    rom = make_lorom_bank0({
        0x8000: bytes([0xC2, 0x20, 0x20, 0x00, 0x81, 0x60]),
        0x8100: bytes([0x60]),
    })
    F = _BankEntry(name='F', start=0x8000)
    G = _BankEntry(name='G', start=0x8100)
    cfg = _BankCfg(bank=0x00, entries=[F, G])
    parsed = [(0x00, 'bank00.cfg', cfg)]

    fixes = detect_and_route(parsed, rom)
    # F has a JSR — skipped. G is a single RTS — no state change, skipped.
    assert fixes == []
    assert cfg.exit_mx_at == []


def test_skips_when_body_has_jsl():
    """Non-leaf: contains JSL. Same reason as JSR."""
    # $8000: REP #$20
    # $8002: JSL $018100  (22 00 81 01)
    # $8006: RTS
    rom = make_lorom_bank0({
        0x8000: bytes([0xC2, 0x20, 0x22, 0x00, 0x81, 0x01, 0x60]),
    })
    F = _BankEntry(name='F', start=0x8000)
    cfg = _BankCfg(bank=0x00, entries=[F])
    parsed = [(0x00, 'bank00.cfg', cfg)]

    fixes = detect_and_route(parsed, rom)
    assert fixes == []


def test_skips_when_no_state_change():
    """A pure leaf with no SEP/REP — exit matches entry, nothing to
    auto-route."""
    # $8000: LDA #$42  (A9 42)         — m=1 entry, so 2-byte LDA
    # $8002: STA $05   (85 05)
    # $8004: RTS       (60)
    rom = make_lorom_bank0({
        0x8000: bytes([0xA9, 0x42, 0x85, 0x05, 0x60]),
    })
    F = _BankEntry(name='F', start=0x8000)
    cfg = _BankCfg(bank=0x00, entries=[F])
    parsed = [(0x00, 'bank00.cfg', cfg)]

    fixes = detect_and_route(parsed, rom)
    assert fixes == []


def test_skips_when_already_cfg_declared():
    """Opt-in cfg `exit_mx_at` directive already exists at F's PC —
    auto-router must respect it and not redundantly add another tuple."""
    rom = make_lorom_bank0({
        0x8000: bytes([0xC2, 0x20, 0x60]),
    })
    F = _BankEntry(name='F', start=0x8000)
    cfg = _BankCfg(
        bank=0x00, entries=[F],
        exit_mx_at=[(0x00, 0x8000, 1, 1)],  # cfg-declared (even if wrong)
    )
    parsed = [(0x00, 'bank00.cfg', cfg)]

    fixes = detect_and_route(parsed, rom)
    assert fixes == []
    # The declared entry is preserved untouched.
    assert cfg.exit_mx_at == [(0x00, 0x8000, 1, 1)]


def test_skips_unnamed_entry():
    """A BankEntry with name=None is scaffolding, not a real function.
    Auto-router skips it."""
    rom = make_lorom_bank0({
        0x8000: bytes([0xC2, 0x20, 0x60]),
    })
    F = _BankEntry(name=None, start=0x8000)
    cfg = _BankCfg(bank=0x00, entries=[F])
    parsed = [(0x00, 'bank00.cfg', cfg)]

    fixes = detect_and_route(parsed, rom)
    assert fixes == []


def test_skips_ambiguous_exit_state():
    """Two RTS paths exiting with different (M, X) — analyzer returns
    None for the disagreeing component. Auto-router must skip."""
    # $8000: LDA $05            (A5 05)   — m=1 entry, 2 bytes
    # $8002: BEQ $8008          (F0 04)   — branch to 8008
    # $8004: REP #$20           (C2 20)   — clear M
    # $8006: RTS                (60)
    # $8007: NOP                (EA)      — padding
    # $8008: RTS                (60)      — exit M=1 (no SEP/REP)
    # One path: $8006 RTS with M=0. Other path: $8008 RTS with M=1.
    rom = make_lorom_bank0({
        0x8000: bytes([0xA5, 0x05, 0xF0, 0x04, 0xC2, 0x20, 0x60, 0xEA, 0x60]),
    })
    F = _BankEntry(name='F', start=0x8000)
    cfg = _BankCfg(bank=0x00, entries=[F])
    parsed = [(0x00, 'bank00.cfg', cfg)]

    fixes = detect_and_route(parsed, rom)
    assert fixes == []
    assert cfg.exit_mx_at == []


def test_skips_when_no_terminator():
    """A function whose decoded graph reaches no RTS/RTL (e.g. infinite
    BRA loop) has no resume point to propagate. Auto-router skips."""
    # $8000: REP #$20
    # $8002: BRA $8002 (80 FE)  — infinite loop, no RTS reached
    rom = make_lorom_bank0({
        0x8000: bytes([0xC2, 0x20, 0x80, 0xFE]),
    })
    F = _BankEntry(name='F', start=0x8000)
    cfg = _BankCfg(bank=0x00, entries=[F])
    parsed = [(0x00, 'bank00.cfg', cfg)]

    fixes = detect_and_route(parsed, rom)
    assert fixes == []


def test_multiple_sites_in_one_bank_each_independent():
    """Two unrelated leaf state-mutators in one bank — both detected."""
    # $8000: REP #$20 ; RTS   — exit M=0 X=1
    # $9000: SEP #$10 ; RTS   — exit M=1 X=1 (SEP #$10 sets X, but X
    #                            was already 1, so no change!)
    # Let's use REP #$10 instead so X actually changes.
    # $8000: REP #$20 ; RTS
    # $9000: REP #$10 ; RTS  — exit M=1 X=0
    rom = make_lorom_bank0({
        0x8000: bytes([0xC2, 0x20, 0x60]),
        0x9000: bytes([0xC2, 0x10, 0x60]),
    })
    F = _BankEntry(name='F', start=0x8000)
    G = _BankEntry(name='G', start=0x9000)
    cfg = _BankCfg(bank=0x00, entries=[F, G])
    parsed = [(0x00, 'bank00.cfg', cfg)]

    fixes = detect_and_route(parsed, rom)
    assert len(fixes) == 2
    fixes_by_pc = {fx.addr16: fx for fx in fixes}
    assert fixes_by_pc[0x8000].exit_m == 0 and fixes_by_pc[0x8000].exit_x == 1
    assert fixes_by_pc[0x9000].exit_m == 1 and fixes_by_pc[0x9000].exit_x == 0
    # Both entries appended.
    assert (0x00, 0x8000, 0, 1) in cfg.exit_mx_at
    assert (0x00, 0x9000, 1, 0) in cfg.exit_mx_at


def test_sep_then_rep_consistent_path_no_change():
    """REP #$20 followed by SEP #$20 — net (M, X) at exit equals entry.
    No state change → skip."""
    # $8000: REP #$20 ; SEP #$20 ; RTS
    rom = make_lorom_bank0({
        0x8000: bytes([0xC2, 0x20, 0xE2, 0x20, 0x60]),
    })
    F = _BankEntry(name='F', start=0x8000)
    cfg = _BankCfg(bank=0x00, entries=[F])
    parsed = [(0x00, 'bank00.cfg', cfg)]

    fixes = detect_and_route(parsed, rom)
    assert fixes == []


def test_sep_only_x_flag():
    """SEP #$10 only sets X — auto-detect from entry (X=0) to exit X=1."""
    # Entry F at M=0 X=0. SEP #$10 ; RTS — sets X=1.
    rom = make_lorom_bank0({
        0x8000: bytes([0xE2, 0x10, 0x60]),
    })
    F = _BankEntry(name='F', start=0x8000, entry_m=0, entry_x=0)
    cfg = _BankCfg(bank=0x00, entries=[F])
    parsed = [(0x00, 'bank00.cfg', cfg)]

    fixes = detect_and_route(parsed, rom)
    assert len(fixes) == 1
    assert fixes[0].entry_m == 0 and fixes[0].entry_x == 0
    assert fixes[0].exit_m == 0 and fixes[0].exit_x == 1
