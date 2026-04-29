"""Phase 6b: emit_bank smoke. Build a synthetic 2-entry bank cfg,
emit the bank source, and assert structural properties."""
from _helpers import make_lorom_bank0  # noqa: E402

from v2.emit_bank import emit_bank, BankEntry  # noqa: E402


def test_two_entry_bank_emits_both_functions():
    """Bank cfg with two entries: a 'init' (LDA, STA, RTS) and a 'tick'
    (NOP, RTS). Emitted bank source must contain both as separate
    void function definitions."""
    rom = make_lorom_bank0({
        0x8000: bytes([0xA9, 0x05, 0x85, 0x00, 0x60]),  # init
        0x8010: bytes([0xEA, 0x60]),                    # tick
    })
    src = emit_bank(rom, bank=0, entries=[
        BankEntry(name="init",  start=0x8000, end=0x8005),
        BankEntry(name="tick",  start=0x8010, end=0x8012),
    ])

    assert "void init(CpuState *cpu)" in src
    assert "void tick(CpuState *cpu)" in src
    # Header was emitted.
    assert "#include \"cpu_state.h\"" in src
    assert "#include \"funcs.h\"" in src


def test_default_func_name_when_none_specified():
    """BankEntry.name = None → emit_function uses bank_BB_AAAA naming."""
    rom = make_lorom_bank0({
        0x8000: bytes([0x60]),  # RTS
    })
    src = emit_bank(rom, bank=0x01, entries=[
        BankEntry(name=None, start=0x8000),
    ])
    assert "void bank_01_8000(CpuState *cpu)" in src


def test_emitted_bank_is_brace_balanced():
    rom = make_lorom_bank0({
        0x8000: bytes([0xA9, 0x05, 0x85, 0x00, 0x60]),
        0x8010: bytes([0xEA, 0x60]),
    })
    src = emit_bank(rom, bank=0, entries=[
        BankEntry(name="a", start=0x8000),
        BankEntry(name="b", start=0x8010),
    ])
    assert src.count("{") == src.count("}"), (
        f"unbalanced braces: {{ x{src.count('{')} vs }} x{src.count('}')}"
    )


def test_entry_order_is_preserved():
    """Entries appear in the same order as supplied."""
    rom = make_lorom_bank0({
        0x8000: bytes([0x60]),
        0x8010: bytes([0x60]),
        0x8020: bytes([0x60]),
    })
    src = emit_bank(rom, bank=0, entries=[
        BankEntry(name="first",  start=0x8000),
        BankEntry(name="second", start=0x8010),
        BankEntry(name="third",  start=0x8020),
    ])
    pos_first = src.find("void first")
    pos_second = src.find("void second")
    pos_third = src.find("void third")
    assert -1 < pos_first < pos_second < pos_third


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
