"""Phase 6c: cfg_loader. Synthetic cfgs and a real bank01.cfg slice
both parse without error."""
import os
import pathlib
import tempfile

from v2.cfg_loader import load_bank_cfg, BankCfg, NameDecl  # noqa: E402
from v2.emit_bank import BankEntry  # noqa: E402


def _write(content: str) -> str:
    """Write `content` to a temp file and return the path."""
    fd, path = tempfile.mkstemp(suffix='.cfg', text=True)
    with os.fdopen(fd, 'w', encoding='utf-8') as f:
        f.write(content)
    return path


def test_minimal_cfg_with_one_func():
    path = _write("""\
bank = 00
func SomeFunc 8000 sig:void()
""")
    try:
        cfg = load_bank_cfg(path)
        assert cfg.bank == 0
        assert len(cfg.entries) == 1
        assert cfg.entries[0].name == 'SomeFunc'
        assert cfg.entries[0].start == 0x8000
        assert cfg.entries[0].end is None
    finally:
        os.unlink(path)


def test_terminal_jsr_call_site_contract_parses():
    path = _write("""\
bank = b3
terminal_jsr 9dac
func SpriteMain 9dac end:9db0 entry_mx:0,0
""")
    try:
        cfg = load_bank_cfg(path)
        assert cfg.terminal_jsr == {0x9DAC}
    finally:
        os.unlink(path)


def test_hle_spc_upload_protocol_mode_parses():
    path = _write("""\
bank = 00
hle_spc_upload 8079 live
func Upload 8079
""")
    try:
        cfg = load_bank_cfg(path)
        assert cfg.hle_spc_upload == {0x8079: 'live'}
    finally:
        os.unlink(path)


def test_func_with_end_directive_parses_end():
    path = _write("""\
bank = 00
func WithEnd 8000 end:806b sig:void() # MANUAL
""")
    try:
        cfg = load_bank_cfg(path)
        assert cfg.entries[0].end == 0x806b
    finally:
        os.unlink(path)


def test_func_with_entry_mx_parses_entry_widths():
    path = _write("""\
bank = 0d
func PalInstr_Wait c595 end:c599 entry_mx:0,0
""")
    try:
        cfg = load_bank_cfg(path)
        assert cfg.entries[0].entry_m == 0
        assert cfg.entries[0].entry_x == 0
    finally:
        os.unlink(path)


def test_entry_mx_at_overrides_generated_func_entry_widths():
    path = _write("""\
bank = 0d
entry_mx_at c595 0 0
func PalInstr_Wait c595 end:c599
""")
    try:
        cfg = load_bank_cfg(path)
        assert cfg.entries[0].entry_m == 0
        assert cfg.entries[0].entry_x == 0
    finally:
        os.unlink(path)


def test_end_at_overrides_generated_func_boundary():
    path = _write("""\
bank = 0d
end_at c5dd c5e4
func PalInstr_ClearPreInstr c5dd end:c61e
""")
    try:
        cfg = load_bank_cfg(path)
        assert cfg.entries[0].end == 0xc5e4
    finally:
        os.unlink(path)


def test_v1_abi_directives_ignored():
    """v2 ignores sig:/ret_y/carry_ret/y_after/x_after/restores_x."""
    path = _write("""\
bank = 00
func A 8000 sig:RetAY(uint8_k,uint8_a) ret_y carry_ret y_after:+2 x_after:+1 restores_x:g_ram[0xdde] # MANUAL
""")
    try:
        cfg = load_bank_cfg(path)
        assert len(cfg.entries) == 1
        assert cfg.entries[0].name == 'A'
        assert cfg.entries[0].start == 0x8000
        # No `end:` so it stays None.
        assert cfg.entries[0].end is None
    finally:
        os.unlink(path)


def test_includes_line_collected():
    path = _write("""\
bank = 00
includes = common_rtl.h smw_rtl.h variables.h funcs.h consts.h
func A 8000
""")
    try:
        cfg = load_bank_cfg(path)
        assert cfg.includes == ['common_rtl.h', 'smw_rtl.h', 'variables.h', 'funcs.h', 'consts.h']
    finally:
        os.unlink(path)


def test_name_decls_collected():
    path = _write("""\
bank = 00
name 02a9e4 FindFreeNormalSpriteSlot_HighPriority
name 01808C ProcessNormalSprites
func MyFunc 8000
""")
    try:
        cfg = load_bank_cfg(path)
        assert len(cfg.names) == 2
        assert cfg.names[0].addr_24 == 0x02a9e4
        assert cfg.names[0].name == 'FindFreeNormalSpriteSlot_HighPriority'
        assert cfg.names[1].addr_24 == 0x01808C
        assert cfg.names[1].name == 'ProcessNormalSprites'
    finally:
        os.unlink(path)


def test_exclude_range_and_data_region():
    path = _write("""\
bank = 00
exclude_range 8FCF 8FD6
data_region 02 b000 b100
func A 8000
""")
    try:
        cfg = load_bank_cfg(path)
        assert cfg.exclude_ranges == [(0x8FCF, 0x8FD6)]
        assert cfg.data_regions == [(0x02, 0xb000, 0xb100)]
    finally:
        os.unlink(path)


def test_ptrcall_targets_preserve_24_bit_values():
    path = _write("""\
bank = 08
indirect_dispatch 852c 2 ptrcall targets:8884B8,91D27F
func A 8000
""")
    try:
        cfg = load_bank_cfg(path)
        auth = cfg.indirect_dispatch[0]
        assert auth['ptr_call'] is True
        assert auth['targets'] == (0x8884B8, 0x91D27F)
    finally:
        os.unlink(path)


def test_ptrcall_targets_keep_16_bit_values_local():
    path = _write("""\
bank = 08
indirect_dispatch 8552 2 ptrcall targets:8569,EB9F
func A 8000
""")
    try:
        cfg = load_bank_cfg(path)
        auth = cfg.indirect_dispatch[0]
        assert auth['targets'] == (0x8569, 0xEB9F)
    finally:
        os.unlink(path)


def test_ptrtail_targets_mark_value_matched_terminal_dispatch():
    path = _write("""\
bank = 00
indirect_dispatch 8000 2 ptrtail targets:019000,029000
func A 8000
""")
    try:
        cfg = load_bank_cfg(path)
        auth = cfg.indirect_dispatch[0]
        assert auth['ptr_call'] is False
        assert auth['pointer_match'] is True
        assert auth['targets'] == (0x019000, 0x029000)
    finally:
        os.unlink(path)


def test_ptrtail_popcall_marks_consumed_incoming_jsr_frame():
    path = _write("""\
bank = 00
indirect_dispatch 8000 2 ptrtail_popcall targets:019000,029000
func A 8000
""")
    try:
        cfg = load_bank_cfg(path)
        auth = cfg.indirect_dispatch[0]
        assert auth['ptr_call'] is False
        assert auth['pointer_match'] is True
        assert auth['popped_call_frame'] is True
    finally:
        os.unlink(path)


def test_rtsstack_targets_mark_internal_stack_dispatch():
    path = _write("""\
bank = bb
indirect_dispatch 8e0d 2 rtsstack targets:BB8001,BB8005
func A 8000
""")
    try:
        cfg = load_bank_cfg(path)
        auth = cfg.indirect_dispatch[0]
        assert auth['rts_stack'] is True
        assert auth['pointer_match'] is False
        assert auth['targets'] == (0xBB8001, 0xBB8005)
    finally:
        os.unlink(path)


def test_missing_bank_line_raises():
    path = _write("""\
func A 8000
""")
    try:
        try:
            load_bank_cfg(path)
        except ValueError as exc:
            assert 'bank' in str(exc)
            return
        assert False, "expected ValueError on missing 'bank = NN'"
    finally:
        os.unlink(path)


def test_real_bank_cfg_parses():
    """Sanity: load the actual SuperMarioWorldRecomp/recomp/bank01.cfg
    via the v1 path-resolved-from-this-tests-dir layout, if available.

    Skipped if the path doesn't exist (e.g. the test is run in a
    snesrecomp-only checkout)."""
    REPO = pathlib.Path(__file__).resolve().parent.parent.parent
    smw_repo = REPO.parent / 'SuperMarioWorldRecomp'
    cfg_path = smw_repo / 'recomp' / 'bank01.cfg'
    if not cfg_path.exists():
        # Don't fail if the parent repo isn't side-by-side; this is a
        # bonus integration check, not a phase gate.
        return
    cfg = load_bank_cfg(str(cfg_path))
    assert cfg.bank == 1
    assert len(cfg.entries) > 0, "bank01.cfg has no func entries?"
    # Spot check: at least one well-known function should appear.
    names = {e.name for e in cfg.entries}
    # ProcessNormalSprites was earlier removed from cfg as a debug step;
    # use a more enduring entry.
    assert any('Spr' in n or 'Normal' in n or 'Pokey' in n for n in names), (
        f"bank01.cfg entries don't contain any expected names; got first 5: "
        f"{sorted(names)[:5]}"
    )


if __name__ == '__main__':
    import sys, traceback
    failed = 0
    for name in [n for n in dir() if n.startswith('test_')]:
        try:
            globals()[name]()
            print(f"  PASS  {name}")
        except Exception:
            failed += 1
            print(f"  FAIL  {name}")
            traceback.print_exc()
    sys.exit(0 if failed == 0 else 1)
