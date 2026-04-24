"""Tail-call X-restore inheritance.

`_detect_x_restore_expr` walks back from RTS/RTL to find an explicit
`LDX DP/ABS ; RTS` pattern. Functions that end in a tail call
(fall-through into the next function, unconditional JMP/BRA/BRL to
another function) have NO RTS in their decoded insns, so the detector
returns None — even when the tail-callee does restore X.

`_augment_cfg_sigs_one_pass` must inherit the tail-callee's x_restore
for such tail-caller functions. Without this, callers of the tail-
caller see X as clobbered after the JSR and emit indexed stores with
X=0 (UNKNOWN), corrupting slot 0 instead of the intended slot.

Canonical failure: SMW GenericGFXRtDraw2Tiles16x16sStacked_Sub at
$019D67 falls through to $019DA9 (which restores X from
CurSpriteProcess before RTS). Callers of the _Sub entry — e.g.
Spr0to13Gfx's `JSR SubSprGfx1` followed by `PLA ; STA SpriteYPosLow,X`
restore — lose X tracking, corrupting slot 0 and drifting the
intended sprite's Y by -0x10/frame.
"""
import pathlib
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / 'recompiler'))

import recomp  # noqa: E402


def _run_augment(rom: bytes, bank: int, funcs: list, sigs: dict):
    cfg = recomp.Config()
    cfg.bank = bank
    cfg.funcs = [(n, a, s, 0, None, None) for n, a, s in funcs]
    cfg.sigs = {((bank << 16) | a): s for n, a, s in funcs}
    cfg.names = {a: n for n, a, s in funcs}
    cfg.skip = set()
    cfg.jsl_dispatch = {}
    cfg.jsl_dispatch_long = {}
    cfg.exclude_ranges = []
    cfg.preserves = {}
    cfg.x_restores = {}
    # Seed with dispatch-table infra the augment pass expects.
    cfg.dispatch_tables = []
    cfg.dispatch_target_addrs = set()
    cfg.dispatch_shape = {}
    # Multi-pass loop — tail-call propagation needs tail-callee's
    # x_restore already computed. First pass sets the callee's entry;
    # second pass inherits into the caller.
    for _ in range(4):
        recomp._augment_cfg_sigs_one_pass(rom, cfg)
    return cfg


def test_fall_through_tail_caller_inherits_x_restore():
    """F at $8000 falls through into F' at $8003. F' does
    `LDX $13BF ; RTS`. F's x_restore must be inherited as g_ram[0x13bf].
    """
    rom = bytearray(0x10000)
    # $8000 F: LDA #$00 ; STA $00           (4 bytes, non-terminal)
    rom[0x0000] = 0xA9; rom[0x0001] = 0x00  # LDA #$00
    rom[0x0002] = 0x85; rom[0x0003] = 0x00  # STA $00
    # $8004 F': LDX $13BF ; RTS               (4 bytes)
    rom[0x0004] = 0xAE; rom[0x0005] = 0xBF; rom[0x0006] = 0x13  # LDX $13BF
    rom[0x0007] = 0x60  # RTS

    cfg = _run_augment(
        rom, bank=0,
        funcs=[('F', 0x8000, 'void()'), ('Fprime', 0x8004, 'void()')],
        sigs={0x8000: 'void()', 0x8004: 'void()'},
    )
    F_full = 0x8000
    Fp_full = 0x8004
    assert Fp_full in cfg.x_restores, (
        f'F prime should have x_restore detected from `LDX $13BF; RTS`. '
        f'x_restores={cfg.x_restores}')
    assert F_full in cfg.x_restores, (
        f'F (tail-caller) should inherit x_restore from F prime. '
        f'x_restores={cfg.x_restores}')
    assert cfg.x_restores[F_full] == cfg.x_restores[Fp_full], (
        f'F inherited restore expr must match F prime. '
        f'F={cfg.x_restores[F_full]!r} Fprime={cfg.x_restores[Fp_full]!r}')


def test_terminal_function_does_not_inherit():
    """F ends in RTS (terminal, no tail call). Must NOT inherit from F'."""
    rom = bytearray(0x10000)
    # $8000 F: LDA #$00 ; RTS                 (3 bytes, terminal)
    rom[0x0000] = 0xA9; rom[0x0001] = 0x00
    rom[0x0002] = 0x60
    # $8003 F': LDX $13BF ; RTS
    rom[0x0003] = 0xAE; rom[0x0004] = 0xBF; rom[0x0005] = 0x13
    rom[0x0006] = 0x60

    cfg = _run_augment(
        rom, bank=0,
        funcs=[('F', 0x8000, 'void()'), ('Fprime', 0x8003, 'void()')],
        sigs={0x8000: 'void()', 0x8003: 'void()'},
    )
    assert 0x8000 not in cfg.x_restores, (
        f'Terminal function F should NOT inherit x_restore. '
        f'x_restores={cfg.x_restores}')
    assert 0x8003 in cfg.x_restores, (
        f'F prime should still have own detected x_restore. '
        f'x_restores={cfg.x_restores}')


def test_fall_through_function_with_ldx_before_end_detects_restore():
    """Function F ends in fall-through (no RTS), but its last X-writer
    is an `LDX $XXXX` just before the fall-through boundary. The
    detector must treat the fall-through boundary as a synthetic exit
    and pick up the LDX restore — so callers see X restored to that
    WRAM source.

    Canonical case: SMW SubSprGfx1 was split by recomp at $019D67 /
    $019DA9. The LDX CurSpriteProcess lives in the $019D67 part before
    the fall-through. Without synthesizing an exit at the function end,
    neither half's x_restore gets detected.
    """
    rom = bytearray(0x10000)
    # $8000 F: STA $05 ; LDX $13BF   (non-terminal, 5 bytes)
    rom[0x0000] = 0x85; rom[0x0001] = 0x05  # STA $05
    rom[0x0002] = 0xAE; rom[0x0003] = 0xBF; rom[0x0004] = 0x13  # LDX $13BF
    # $8005 F': LDA $10 ; RTS        (F' doesn't touch X)
    rom[0x0005] = 0xA5; rom[0x0006] = 0x10
    rom[0x0007] = 0x60

    cfg = _run_augment(
        rom, bank=0,
        funcs=[('F', 0x8000, 'void()'), ('Fprime', 0x8005, 'void()')],
        sigs={0x8000: 'void()', 0x8005: 'void()'},
    )
    assert 0x8000 in cfg.x_restores, (
        f'F ends in fall-through with LDX $13BF as last X-writer — '
        f'detector should synthesize an exit. x_restores={cfg.x_restores}')
    assert 'g_ram[0x13bf]' in cfg.x_restores[0x8000], (
        f'Expected g_ram[0x13bf] restore, got {cfg.x_restores[0x8000]!r}')


def test_fall_through_chain_two_levels():
    """F -> F' -> F''. F and F' both fall through. F'' has LDX restore.
    Both F and F' must inherit."""
    rom = bytearray(0x10000)
    # $8000 F: STA $00 (2 bytes, non-terminal)
    rom[0x0000] = 0x85; rom[0x0001] = 0x00
    # $8002 F': STA $01 (2 bytes, non-terminal)
    rom[0x0002] = 0x85; rom[0x0003] = 0x01
    # $8004 F'': LDX $13BF ; RTS
    rom[0x0004] = 0xAE; rom[0x0005] = 0xBF; rom[0x0006] = 0x13
    rom[0x0007] = 0x60

    cfg = _run_augment(
        rom, bank=0,
        funcs=[('F', 0x8000, 'void()'),
               ('Fprime', 0x8002, 'void()'),
               ('Fpp', 0x8004, 'void()')],
        sigs={0x8000: 'void()', 0x8002: 'void()', 0x8004: 'void()'},
    )
    for addr in (0x8000, 0x8002, 0x8004):
        assert addr in cfg.x_restores, (
            f'Each fall-through link must carry the x_restore. '
            f'Missing 0x{addr:x}; x_restores={cfg.x_restores}')
    assert (cfg.x_restores[0x8000]
            == cfg.x_restores[0x8002]
            == cfg.x_restores[0x8004]), (
        f'All three must share the same restore expr; '
        f'got {cfg.x_restores}')
