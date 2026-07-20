"""Stable generated-bank translation-unit sharding."""

import pathlib
import tempfile

from v2.translation_units import (
    split_bank_translation_units,
    write_bank_translation_units,
)


SOURCE = """/* generated */
#include "cpu_state.h"

/* Forward declarations for in-bank entries. */
RecompReturn bank_00_8000_M1X1(CpuState *cpu);
RecompReturn bank_00_8800_M1X1(CpuState *cpu);

RecompReturn bank_00_8000_M1X1(CpuState *cpu) {
  return bank_00_8800_M1X1(cpu);
}

RecompReturn bank_00_8800_M1X1(CpuState *cpu) {
  return RECOMP_RETURN_NORMAL;
}

void ResetHandler(CpuState *cpu) {
  RecompReturn r = bank_00_8000_M1X1(cpu);
}
"""

SYMBOL_PCS = {
    "bank_00_8000": 0x8000,
    "bank_00_8800": 0x8800,
    "ResetHandler": 0x8000,
}


def test_large_bank_splits_by_stable_pc_range():
    parts = split_bank_translation_units(
        SOURCE, 0, SYMBOL_PCS, threshold_bytes=0, pc_span=0x800)
    assert set(parts) == {"bank00_part00_v2.c", "bank00_part01_v2.c"}
    assert "bank_00_8000_M1X1(CpuState *cpu) {" in \
        parts["bank00_part00_v2.c"]
    assert "void ResetHandler(CpuState *cpu) {" in \
        parts["bank00_part00_v2.c"]
    assert "bank_00_8800_M1X1(CpuState *cpu) {" in \
        parts["bank00_part01_v2.c"]
    assert "bank_00_8000_M1X1(CpuState *cpu) {" not in \
        parts["bank00_part01_v2.c"]


def test_shard_embeds_only_referenced_variant_declarations():
    parts = split_bank_translation_units(
        SOURCE, 0, SYMBOL_PCS, threshold_bytes=0, pc_span=0x800)
    part0 = parts["bank00_part00_v2.c"]
    assert "RecompReturn bank_00_8800_M1X1(CpuState *cpu);" in part0
    part1_preamble = parts["bank00_part01_v2.c"].split(
        "RecompReturn bank_00_8800_M1X1(CpuState *cpu) {", 1)[0]
    assert "bank_00_8000_M1X1(CpuState *cpu);" not in part1_preamble


def test_small_bank_keeps_monolithic_filename_and_adds_call_declarations():
    parts = split_bank_translation_units(
        SOURCE, 0, SYMBOL_PCS, threshold_bytes=len(SOURCE) + 1,
        pc_span=0x800)
    assert set(parts) == {"bank00_v2.c"}
    generated = parts["bank00_v2.c"]
    body = generated.split(
        "RecompReturn bank_00_8000_M1X1(CpuState *cpu) {", 1)[0]
    assert "RecompReturn bank_00_8800_M1X1(CpuState *cpu);" in body


def test_monolithic_bank_declares_cross_bank_dispatch_target():
    source = SOURCE.replace(
        "return bank_00_8800_M1X1(cpu);",
        "return bank_82_80B4_M1X1(cpu);")
    parts = split_bank_translation_units(
        source, 0, SYMBOL_PCS, threshold_bytes=len(source) + 1,
        pc_span=0x800)
    body = parts["bank00_v2.c"].split(
        "RecompReturn bank_00_8000_M1X1(CpuState *cpu) {", 1)[0]
    assert "RecompReturn bank_82_80B4_M1X1(CpuState *cpu);" in body


def test_writer_removes_stale_bank_shape():
    with tempfile.TemporaryDirectory() as directory:
        out_dir = pathlib.Path(directory)
        stale = out_dir / "bank00_v2.c"
        stale.write_text("old", encoding="utf-8")
        names, changed = write_bank_translation_units(
            out_dir, 0, SYMBOL_PCS, SOURCE,
            threshold_bytes=0, pc_span=0x800)
        assert set(names) == {"bank00_part00_v2.c", "bank00_part01_v2.c"}
        assert changed == 3
        assert not stale.exists()

        names, changed = write_bank_translation_units(
            out_dir, 0, SYMBOL_PCS, SOURCE,
            threshold_bytes=len(SOURCE) + 1, pc_span=0x800)
        assert names == ("bank00_v2.c",)
        assert changed == 3
        assert not list(out_dir.glob("bank00_part*_v2.c"))
