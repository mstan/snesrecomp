"""Tests for tools/smwdisx_compare.py harness.

The harness lives in the game repo (tools/smwdisx_compare.py), not the
framework. Testing it from the framework tests keeps the contract
visible across re-runs.

Skipped when the test is run against a game that doesn't have this
harness or lacks SMWDisX assets.
"""
import importlib.util
import pathlib
import sys


def _load_harness():
    framework_root = pathlib.Path(__file__).resolve().parent.parent
    game_root = framework_root.parent
    script = game_root / 'tools' / 'smwdisx_compare.py'
    smwdisx_dir = game_root / 'SMWDisX'
    sym_file = smwdisx_dir / 'SMW_U.sym'
    if not (script.exists() and sym_file.exists()):
        return None
    sys.path.insert(0, str(framework_root / 'recompiler'))
    spec = importlib.util.spec_from_file_location(
        'smwdisx_compare', str(script))
    mod = importlib.util.module_from_spec(spec)
    # The harness imports `recomp` and `snes65816` relative to
    # snesrecomp/recompiler — already on sys.path.
    sys.modules['smwdisx_compare'] = mod
    spec.loader.exec_module(mod)
    return mod


# ---------------------------------------------------------------------------
# _insn_size: pin computed sizes for common 65816 instruction shapes
# ---------------------------------------------------------------------------

def test_insn_size_one_byte_mnems():
    sfc = _load_harness()
    if sfc is None:
        return
    # Pure implied/accumulator-only: INX, DEX, RTS, CLC, etc.
    assert sfc._insn_size('INX', '', '') == 1
    assert sfc._insn_size('RTS', '', '') == 1
    assert sfc._insn_size('PHA', '', '') == 1
    # Accumulator form of shift/rotate/inc/dec: ASL A, ROL A, INC A.
    assert sfc._insn_size('ASL', '', 'A') == 1
    assert sfc._insn_size('INC', '', 'A') == 1


def test_insn_size_branches():
    sfc = _load_harness()
    if sfc is None:
        return
    for b in ('BEQ', 'BNE', 'BCS', 'BCC', 'BMI', 'BPL', 'BVS', 'BVC', 'BRA'):
        assert sfc._insn_size(b, '', 'label') == 2, b
    assert sfc._insn_size('BRL', '', 'label') == 3
    assert sfc._insn_size('PER', '', 'label') == 3


def test_insn_size_width_suffix_wins():
    sfc = _load_harness()
    if sfc is None:
        return
    # .B / .W / .L suffix always determines the encoding size.
    assert sfc._insn_size('LDA', '.B', '$00') == 2
    assert sfc._insn_size('LDA', '.W', '$1234') == 3
    assert sfc._insn_size('LDA', '.L', '$123456') == 4
    assert sfc._insn_size('STA', '.B', '$FF') == 2
    assert sfc._insn_size('STA', '.W', '$1234,X') == 3
    assert sfc._insn_size('STA', '.L', '$123456,X') == 4


def test_insn_size_immediate_no_suffix():
    sfc = _load_harness()
    if sfc is None:
        return
    # Byte-width immediate without suffix.
    assert sfc._insn_size('LDA', '', '#$00') == 2
    assert sfc._insn_size('LDX', '', '#$FF') == 2
    # Word-width immediate without suffix.
    assert sfc._insn_size('LDA', '', '#$0000') == 3
    assert sfc._insn_size('LDA', '', '#$FFFF') == 3


def test_insn_size_jumps_and_calls():
    sfc = _load_harness()
    if sfc is None:
        return
    assert sfc._insn_size('JSR', '', 'label') == 3
    assert sfc._insn_size('JMP', '', 'label') == 3
    assert sfc._insn_size('JMP', '', '(label)') == 3
    assert sfc._insn_size('JMP', '', '[label]') == 4
    assert sfc._insn_size('JSL', '', 'label') == 4
    assert sfc._insn_size('JML', '', 'label') == 4


def test_insn_size_imm_only_fixed_2():
    sfc = _load_harness()
    if sfc is None:
        return
    # REP / SEP / COP / BRK / WDM are always 2-byte imm-only.
    assert sfc._insn_size('REP', '', '#$30') == 2
    assert sfc._insn_size('SEP', '', '#$20') == 2
    assert sfc._insn_size('BRK', '', '#$00') == 2


# ---------------------------------------------------------------------------
# _eval_ver_predicate: U-ROM selects english/lores/ntsc/console branches
# ---------------------------------------------------------------------------

def test_eval_ver_predicate_u_rom_truth_table():
    sfc = _load_harness()
    if sfc is None:
        return
    assert sfc._eval_ver_predicate('ver_is_english(!_VER)') is True
    assert sfc._eval_ver_predicate('ver_is_japanese(!_VER)') is False
    assert sfc._eval_ver_predicate('ver_is_lores(!_VER)') is True
    assert sfc._eval_ver_predicate('ver_is_hires(!_VER)') is False
    assert sfc._eval_ver_predicate('ver_is_ntsc(!_VER)') is True
    assert sfc._eval_ver_predicate('ver_is_pal(!_VER)') is False
    assert sfc._eval_ver_predicate('ver_is_console(!_VER)') is True
    assert sfc._eval_ver_predicate('ver_is_arcade(!_VER)') is False


# ---------------------------------------------------------------------------
# _expand_macro: BorW/WorB/WorL_X/LorW_X/LorW for U ROM
# ---------------------------------------------------------------------------

def test_expand_macro_u_rom():
    sfc = _load_harness()
    if sfc is None:
        return
    # %BorW(LDA, $1234) → LDA.W $1234 for U (byte for J).
    mnem, suffix, operand = sfc._expand_macro('BorW', 'LDA, $1234')
    assert (mnem, suffix, operand) == ('LDA', '.W', '$1234')
    # %WorB(STA, Addr) → STA.B Addr for U (word for J).
    mnem, suffix, operand = sfc._expand_macro('WorB', 'STA, Addr')
    assert (mnem, suffix, operand) == ('STA', '.B', 'Addr')
    # %WorL_X(LDA, Table) → LDA.L Table,X for U (word,X for J).
    mnem, suffix, operand = sfc._expand_macro('WorL_X', 'LDA, Table')
    assert (mnem, suffix, operand) == ('LDA', '.L', 'Table,X')
    # %LorW_X(LDA, Table) → LDA.W Table,X for U (long,X for J).
    mnem, suffix, operand = sfc._expand_macro('LorW_X', 'LDA, Table')
    assert (mnem, suffix, operand) == ('LDA', '.W', 'Table,X')
    # %LorW(LDA, Addr) → LDA.W Addr for U (long for J).
    mnem, suffix, operand = sfc._expand_macro('LorW', 'LDA, Addr')
    assert (mnem, suffix, operand) == ('LDA', '.W', 'Addr')


def test_expand_macro_unknown_returns_none():
    sfc = _load_harness()
    if sfc is None:
        return
    # %insert_empty / %DMASettings / etc. aren't mnemonic expansions.
    assert sfc._expand_macro('insert_empty', '$3E,$41,$41,$41,$41') is None
    assert sfc._expand_macro('DMASettings', 'a, b, c, d') is None


# ---------------------------------------------------------------------------
# parse_bank_mnems: anchor-reset from CODE_XXADDR labels keeps drift bounded
# ---------------------------------------------------------------------------

def test_parse_bank_mnems_covers_known_code():
    sfc = _load_harness()
    if sfc is None:
        return
    mmap = sfc.parse_bank_mnems('05')
    # BufferScrollingTiles_Layer1_Init at $0588EC starts with SEP #$30.
    # SMWDisX bank_05.asm:1062 confirms.
    assert mmap.get(0x0588EC) == ('SEP', ''), (
        f'expected SEP at $0588EC, got {mmap.get(0x0588EC)!r}'
    )
    # Next instruction after SEP #$30 (2 bytes) is LDA.W LevelModeSetting
    # at $0588EE.
    assert mmap.get(0x0588EE) == ('LDA', '.W'), (
        f'expected LDA.W at $0588EE, got {mmap.get(0x0588EE)!r}'
    )
    # JSL ExecutePtrLong at $0588F1 (LDA.W = 3 bytes, so +3 from LDA).
    assert mmap.get(0x0588F1) == ('JSL', ''), (
        f'expected JSL at $0588F1, got {mmap.get(0x0588F1)!r}'
    )


def test_parse_bank_mnems_ignores_data_regions():
    sfc = _load_harness()
    if sfc is None:
        return
    mmap = sfc.parse_bank_mnems('05')
    # The dispatch table immediately after JSL ExecutePtrLong at
    # $0588F4..$058974 is all `dl CODE_XXXX` data. None of these
    # addresses should have mnemonics in the map.
    data_range = range(0x0588F4, 0x058975)
    present = [a for a in data_range if a in mmap]
    assert not present, (
        f'parser classified SMWDisX data bytes as code: {present!r}'
    )


# ---------------------------------------------------------------------------
# _mnems_agree: JML == JMP (long-mode equivalence)
# ---------------------------------------------------------------------------

def test_mnems_agree_exact_match():
    sfc = _load_harness()
    if sfc is None:
        return
    assert sfc._mnems_agree('LDA', 'LDA') is True
    assert sfc._mnems_agree('STZ', 'STZ') is True


def test_mnems_agree_jml_jmp_equivalent():
    sfc = _load_harness()
    if sfc is None:
        return
    # SMWDisX writes JML for opcode $5C (long-mode JMP); our decoder
    # emits JMP with LONG mode.
    assert sfc._mnems_agree('JML', 'JMP') is True
    assert sfc._mnems_agree('JMP', 'JML') is True


def test_mnems_agree_real_mismatch():
    sfc = _load_harness()
    if sfc is None:
        return
    assert sfc._mnems_agree('LDA', 'STA') is False
    assert sfc._mnems_agree('SEP', 'REP') is False


# ---------------------------------------------------------------------------
# check_function: synthetic mismatch triggers FAIL with useful diagnostic
# ---------------------------------------------------------------------------

def test_check_function_flags_synthetic_mnem_mismatch():
    sfc = _load_harness()
    if sfc is None:
        return
    # Run the harness on a known-good function but with a mnem_map that
    # claims a different instruction at the entry address. Harness must
    # FAIL and report the mismatch.
    import recomp
    from snes65816 import load_rom
    from pathlib import Path

    framework_root = pathlib.Path(__file__).resolve().parent.parent
    game_root = framework_root.parent
    rom = load_rom(str(game_root / 'smw.sfc'))
    cfgs = sfc.load_cfgs(rom)
    if '05' not in cfgs:
        return
    cfg = cfgs['05']
    labels = sfc.load_symbols()
    mmap = sfc.parse_bank_mnems('05')
    # BufferScrollingTiles_Layer1_Init at $0588EC really has SEP.
    # Inject a fake LDA claim at that address.
    fake = dict(mmap)
    fake[0x0588EC] = ('LDA', '.B')
    res = sfc.check_function(
        rom, cfg, labels, fake,
        'BufferScrollingTiles_Layer1_Init', 0x88EC, 0x8955, None,
    )
    assert res.status == 'FAIL', (
        f'harness failed to detect synthetic mnem mismatch; '
        f'got status={res.status!r} reason={res.reason!r}'
    )
    assert 'mnem mismatch' in res.reason
    assert 'SMWDisX=LDA' in res.reason
    assert 'ours=SEP' in res.reason
