"""L3 per-function behavioral-equivalence tests, input-injection style.

Each test declares a minimal input contract (WRAM bytes + CPU regs),
invokes the function on both the recompiled binary (direct C call) and
the oracle (65816 interpreter, same ROM), and diffs a declared output
region. No savestate dependency — the inputs ARE the fixture.

Green tests are regression guards for bugs we fixed. @known_red tests
pin bugs we haven't — the decorator raises if an expected-red test
unexpectedly passes, prompting you to graduate it to green.
"""
import sys
import pathlib

THIS_DIR = pathlib.Path(__file__).parent
sys.path.insert(0, str(THIS_DIR))

from harness import run_stubbed, known_red, format_stubbed_diff  # noqa: E402


def test_HandleLevelTileAnimations_three_times_pattern():
    """Pin the 3N multiplier fix (snesrecomp 252a2a0).

    ROM at $05:BB3B does: LDA EffFrame ; AND #$07 ; STA _0 ; ASL A ; ADC _0
    expecting A = 3 * (EffFrame & 7) — used as a 6-byte-stride index
    into the $05:B93B pointer table. The function writes three words to
    $0D7C/7E/80 (Gfx33DestAddr C/B/A).

    Before the fix, ADC _0 folded to the post-ASL A variable, computing
    4N instead of 3N and reading misaligned entries. The observable was
    $0D7C/7E/80 = $0101 (garbage) instead of the correct table words.

    With EffFrame=3, we expect table entries at offset 3*3*2 = 18 = $12
    from the 3 base tables ($05:B93B/B93D/B93F).
    """
    diffs = run_stubbed(
        name='HandleLevelTileAnimations',
        inputs={0x0014: 0x03},       # EffFrame
        cpu={'db': 0x00, 'dp': 0x0000, 'pb': 0x05, 'e': 0, 'p': 0x30},
        # Writes both $D7C/7E/80 (Gfx33DestAddr C/B/A) and $D76/78/7A
        # (the loop body's GetAnimatedTile storage). Full output window
        # is $D76..$D81, 12 bytes.
        expected_writes=[(0x0D76, 12)],
        emu_ret='rtl',
    )
    assert not diffs, (
        f'HandleLevelTileAnimations(EffFrame=3) output diverges:\n'
        f'{format_stubbed_diff(diffs)}'
    )


def test_HandleLevelTileAnimations_eff_frame_zero():
    """EffFrame=0 is the degenerate case: 3*0=0, reads the first entry
    of each table. Good coverage for 'does the function work when
    EffFrame & 7 is zero' — an edge the 3N bug wouldn't have caught
    (0*3 == 0*4)."""
    diffs = run_stubbed(
        name='HandleLevelTileAnimations',
        inputs={0x0014: 0x00},
        cpu={'db': 0x00, 'dp': 0x0000, 'pb': 0x05, 'e': 0, 'p': 0x30},
        # Writes both $D7C/7E/80 (Gfx33DestAddr C/B/A) and $D76/78/7A
        # (the loop body's GetAnimatedTile storage). Full output window
        # is $D76..$D81, 12 bytes.
        expected_writes=[(0x0D76, 12)],
        emu_ret='rtl',
    )
    assert not diffs, format_stubbed_diff(diffs)


def test_UploadPlayerGFX_populates_bg_chr_regions():
    """Pins the BG1 chr / OBJ chr shortfall observed in-game (missing
    ground tiles + single-column bushes).

    UploadPlayerGFX at $00:A300 fires 1-2 palette DMAs + 1 7F upload
    DMA + two count-driven loops that write to VRAM $6000 and $6100
    regions. Each loop iterates while X < PlayerGfxTileCount ($0D84),
    incrementing X by 2 each iteration. Count=4 means 2 iterations per
    loop, each a 0x40-byte DMA -> 0x80 bytes per loop.

    Inputs:
      $0D82 (word) palette ptr  -> arbitrary; we point at WRAM[$0200]
      $0D84 (byte) count = 4    -> 2 iterations per loop
      $0D85..$0D88  — 4 bytes = 2 words of DynGfxTilePtr entries
      $0D8F..$0D92  — 4 bytes = 2 words of DynGfxTilePtr+$0A entries
      $0D99 (word) DynGfxTile7FPtr -> WRAM[$0400]
      WRAM $0100..$04FF — data the DMAs will copy into VRAM

    Output contract: VRAM $6000 and $6100 regions (0x100 bytes each =
    0x80 words each) should match between recomp and the interpreter.

    Currently RED: recomp writes fewer words to these regions than the
    interpreter does, believed to be a codegen bug in the $0D84
    comparison width (8-bit vs 16-bit).
    """
    inputs = {
        # Palette source (1x palette DMA, 0x14 bytes from WRAM)
        0x0D82: bytes([0x00, 0x02]),  # ptr = $0200
        # Counter — word. Set low byte = 4, high byte = 0. ROM reads as
        # 16-bit Y; recomp reads byte at $0D84. Behavior should match
        # because high byte = 0.
        0x0D84: bytes([0x04, 0x00]),
        # DynGfxTilePtr entries: count=4 -> 2 entries read
        0x0D85: bytes([0x00, 0x01,    # entry 0 = $0100
                       0x80, 0x01]),  # entry 1 = $0180
        # DynGfxTilePtr+$0A entries
        0x0D8F: bytes([0x00, 0x03,    # = $0300
                       0x80, 0x03]),  # = $0380
        # DynGfxTile7FPtr
        0x0D99: bytes([0x00, 0x04]),  # ptr = $0400
        # Source data (arbitrary but distinguishable per region)
        0x0100: bytes(range(0x40)) * 2,    # 0x80 bytes of 0x00..0x3f pattern
        0x0200: bytes([0xAA] * 0x14),       # palette — 0x14 bytes of 0xAA
        0x0300: bytes(range(0x40, 0x80)) * 2,
        0x0380: bytes(range(0x80, 0xC0)) * 2,
        0x0400: bytes([0xCC] * 0x20),       # 7F upload — 0x20 bytes
    }
    # MarioGFXDMA is called from the NMI handler which has already done
    # SEP #$30 — AXY all 8-bit at entry. The function itself does REP #$20
    # internally to widen A. Entering with x=0 would make LDX.B #$04
    # consume an extra byte (3-byte instruction in 16-bit-X mode),
    # derailing subsequent decoding.
    cpu = {'db': 0x00, 'dp': 0x0000, 'pb': 0x00, 'e': 0, 'p': 0x30}
    diffs = run_stubbed(
        name='UploadPlayerGFX',
        inputs=inputs,
        cpu=cpu,
        expected_writes=[
            ('vram', 0x6000, 0x80),   # first loop's BG chr region, 0x80 words
            ('vram', 0x6100, 0x80),   # second loop's, 0x80 words
            ('vram', 0x67F0, 0x10),   # 7F upload region
        ],
        emu_ret='rts',
    )
    assert not diffs, (
        f'UploadPlayerGFX VRAM output diverges:\n{format_stubbed_diff(diffs)}'
    )


if __name__ == '__main__':
    tests = [
        'test_HandleLevelTileAnimations_three_times_pattern',
        'test_HandleLevelTileAnimations_eff_frame_zero',
        'test_UploadPlayerGFX_populates_bg_chr_regions',
    ]
    for tname in tests:
        fn = globals()[tname]
        try:
            fn()
            print(f'PASS  {tname}')
        except AssertionError as e:
            print(f'FAIL  {tname}:\n{str(e)[:2000]}')
