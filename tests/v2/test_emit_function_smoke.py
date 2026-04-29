"""Phase 6a: emit_function end-to-end smoke. Drives the v2 pipeline
on synthetic ROM fixtures and asserts the emitted C function source
has the expected structural shape."""
from _helpers import make_lorom_bank0  # noqa: E402

from v2.emit_function import emit_function  # noqa: E402


def test_linear_function_signature_and_return():
    """A trivial LDA #$05; STA $00; RTS function.

    Expected shape:
        void bank_00_8000(CpuState *cpu) {
          L_8000_M1X1:
            uint8 _v1 = 0x5;
            cpu->A = _v1;
            uint16 _v2 = (uint16)cpu->A;
            cpu_write8(cpu, 0x7E, ..., _v2);
            return; /* RTS */
          return;
        }
    """
    rom = make_lorom_bank0({
        0x8000: bytes([0xA9, 0x05, 0x85, 0x00, 0x60]),
    })
    src = emit_function(rom, bank=0, start=0x8000, entry_m=1, entry_x=1)

    assert "void bank_00_8000(CpuState *cpu)" in src
    assert "L_8000_M1X1:" in src
    assert "cpu->A" in src
    assert "cpu_write8" in src
    assert "return;" in src


def test_cond_branch_emits_label_targets():
    """BCS forks the cfg; expect two labels and a conditional goto."""
    rom = make_lorom_bank0({
        0x8000: bytes([
            0xB0, 0x02,  # BCS $8004
            0xEA,        # NOP at $8002 (fall-through)
            0x60,        # RTS at $8003
            0xEA,        # NOP at $8004 (taken target)
            0x60,        # RTS at $8005
        ]),
    })
    src = emit_function(rom, bank=0, start=0x8000, entry_m=1, entry_x=1)

    # Both target labels present.
    assert "L_8002_M1X1:" in src
    assert "L_8004_M1X1:" in src

    # Conditional goto on the carry flag.
    assert "cpu->_flag_C" in src
    assert "goto L_8004_M1X1" in src


def test_mode_split_emits_two_labels_per_pc():
    """Two predecessors with different (m, x) reaching same PC -> two
    distinct labels in the emitted source."""
    rom = make_lorom_bank0({
        0x8000: bytes([0xB0, 0x0A, 0xC2, 0x30, 0x80, 0x06]),  # BCS $800C; REP #$30; BRA $800C
        0x800C: bytes([0xEA, 0x60]),                          # NOP; RTS
    })
    src = emit_function(rom, bank=0, start=0x8000, entry_m=1, entry_x=1)

    # Two labels at $800C, one per reaching mode-state.
    assert "L_800C_M1X1:" in src
    assert "L_800C_M0X0:" in src


def test_function_body_is_brace_balanced():
    """Sanity: every emitted function source has balanced { }."""
    rom = make_lorom_bank0({
        0x8000: bytes([0xA9, 0x05, 0x85, 0x00, 0x60]),
    })
    src = emit_function(rom, bank=0, start=0x8000, entry_m=1, entry_x=1)
    open_count = src.count("{")
    close_count = src.count("}")
    assert open_count == close_count, (
        f"unbalanced braces: {{ x{open_count} vs }} x{close_count}\n{src}"
    )


def test_jsr_emits_function_call():
    """JSR $8010 -> emits bank_00_8010(cpu);"""
    rom = make_lorom_bank0({
        0x8000: bytes([
            0x20, 0x10, 0x80,   # JSR $8010
            0x60,               # RTS at $8003
        ]),
        0x8010: bytes([0x60]),  # RTS at $8010 (callee body — discovered as part of decode? no: JSR doesn't follow into the callee in v2 decoder, target is opaque)
    })
    src = emit_function(rom, bank=0, start=0x8000, entry_m=1, entry_x=1)
    assert "bank_00_8010(cpu);" in src


def test_alu_emits_carry_chain_for_adc():
    """ADC #$10 -> emits cpu->_flag_C in carry chain."""
    rom = make_lorom_bank0({
        0x8000: bytes([0x69, 0x10, 0x60]),  # ADC #$10; RTS
    })
    src = emit_function(rom, bank=0, start=0x8000, entry_m=1, entry_x=1)
    assert "cpu->_flag_C" in src


def test_rep_sep_emits_p_updates_with_mirrors():
    """REP #$30 / SEP #$30 emit P-byte updates + cpu_p_to_mirrors call."""
    rom = make_lorom_bank0({
        0x8000: bytes([0xC2, 0x30, 0xE2, 0x30, 0x60]),  # REP, SEP, RTS
    })
    src = emit_function(rom, bank=0, start=0x8000, entry_m=1, entry_x=1)
    assert "cpu->P" in src
    assert "cpu_p_to_mirrors" in src
    assert "0x30" in src


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
