import json
import pathlib
import re
import subprocess
import sys
from collections import Counter

from v2.cfg_loader import load_bank_cfg
from v2.program_analysis import VariantKey
from v2.program_emit import discover_host_roots


REPO = pathlib.Path(__file__).resolve().parents[2]


def _fixture(tmp_path, target_opcode=0x00):
    rom = bytearray([0xFF] * 0x8000)
    rom[0:4] = bytes([0x20, 0x10, 0x80, 0x60])  # JSR $8010; RTS
    rom[0x10] = target_opcode
    rom[0x11] = 0x60
    rom[0x7FFC:0x7FFE] = bytes([0x00, 0x80])
    rom[0x7FEA:0x7FEC] = bytes([0xFF, 0xFF])
    rom[0x7FEE:0x7FF0] = bytes([0xFF, 0xFF])
    rom_path = tmp_path / "game.sfc"
    rom_path.write_bytes(rom)
    cfg_dir = tmp_path / "recomp"
    cfg_dir.mkdir()
    (cfg_dir / "bank00.cfg").write_text(
        "bank = 00\nfunc I_RESET 8000 end:8004 entry_mx:1,1\n",
        encoding="utf-8")
    return rom_path, cfg_dir, tmp_path / "gen"


def _run(rom_path, cfg_dir, out_dir):
    return subprocess.run([
        sys.executable, str(REPO / "tools" / "v2_emit.py"),
        "--rom", str(rom_path), "--cfg-dir", str(cfg_dir),
        "--out-dir", str(out_dir), "--no-host-root-scan",
    ], text=True, capture_output=True)


def test_manifest_emitter_keeps_structural_target_as_lle(tmp_path):
    rom_path, cfg_dir, out_dir = _fixture(tmp_path, target_opcode=0x00)
    result = _run(rom_path, cfg_dir, out_dir)
    assert result.returncode == 0, result.stdout + result.stderr

    source = (out_dir / "bank00_v2.c").read_text(encoding="utf-8")
    dispatch = (out_dir / "dispatch_v2.c").read_text(encoding="utf-8")
    manifest = json.loads(
        (out_dir / "program_manifest.json").read_text(encoding="utf-8"))

    # A call whose return M/X cannot be proven tiers the caller to LLE too;
    # preserving the caller width would be a speculative AOT decode.
    assert "I_RESET_M1X1" not in source
    assert "bank_00_8010_M1X1" not in source
    assert "0x008000u, { NULL, NULL, NULL, NULL }" in dispatch
    assert "0x008010u, { NULL, NULL, NULL, NULL }" in dispatch
    assert manifest["nodes"]["008000:M1X1"]["disposition"] == "lle_only"
    assert "unproven_callee_exit" in \
        manifest["nodes"]["008000:M1X1"]["reasons"]
    assert manifest["nodes"]["008010:M1X1"]["disposition"] == "lle_only"


def test_all_lle_host_alias_still_links_to_authoritative_dispatch(tmp_path):
    rom_path, cfg_dir, out_dir = _fixture(tmp_path, target_opcode=0x00)
    (cfg_dir / "funcs.h").write_text(
        "void I_RESET(CpuState *cpu);  /* $00:8000 alias */\n",
        encoding="utf-8")

    result = _run(rom_path, cfg_dir, out_dir)

    assert result.returncode == 0, result.stdout + result.stderr
    source = (out_dir / "bank00_v2.c").read_text(encoding="utf-8")
    assert "void I_RESET(CpuState *cpu)" in source
    assert "I_RESET_M" not in source
    assert source.count("interp_tier_dispatch(cpu, 0x008000u)") == 5


def test_all_lle_interrupt_alias_uses_rti_aware_dispatch(tmp_path):
    rom_path, cfg_dir, out_dir = _fixture(tmp_path, target_opcode=0x00)
    rom = bytearray(rom_path.read_bytes())
    rom[0x7FEA:0x7FEC] = bytes([0x00, 0x80])  # native NMI -> $00:8000
    rom_path.write_bytes(rom)
    (cfg_dir / "funcs.h").write_text(
        "void Interrupt_NMI(CpuState *cpu);  /* $00:8000 alias */\n",
        encoding="utf-8")

    result = _run(rom_path, cfg_dir, out_dir)

    assert result.returncode == 0, result.stdout + result.stderr
    source = (out_dir / "bank00_v2.c").read_text(encoding="utf-8")
    assert "void Interrupt_NMI(CpuState *cpu)" in source
    assert source.count(
        "interp_tier_dispatch_interrupt(cpu, 0x008000u)") == 5


def test_aot_interrupt_tail_to_lle_preserves_rti_boundary(tmp_path):
    rom_path, cfg_dir, out_dir = _fixture(tmp_path, target_opcode=0x00)
    rom = bytearray(rom_path.read_bytes())
    # Native NMI wrapper changes widths, then tail-transfers to a body that is
    # deliberately LLE-only.  This is Super Metroid's $00:9583 -> $80:9589
    # shape, expressed without any title-specific address or hint.
    rom[0:6] = bytes([0xC2, 0x30, 0x5C, 0x10, 0x80, 0x80])
    rom[0x10] = 0x00  # BRK keeps the tail body structural/LLE-only.
    rom[0x7FEA:0x7FEC] = bytes([0x00, 0x80])
    rom_path.write_bytes(rom)
    (cfg_dir / "bank00.cfg").write_text(
        "bank = 00\nfunc Interrupt_NMI 8000 end:8006 entry_mx:1,1\n",
        encoding="utf-8")
    (cfg_dir / "funcs.h").write_text(
        "void Interrupt_NMI(CpuState *cpu);  /* $00:8000 alias */\n",
        encoding="utf-8")

    result = _run(rom_path, cfg_dir, out_dir)

    assert result.returncode == 0, result.stdout + result.stderr
    source = (out_dir / "bank00_v2.c").read_text(encoding="utf-8")
    assert "RecompReturn Interrupt_NMI_M1X1" in source
    assert "interp_tier_dispatch_tail(cpu, 0x808010u" in source
    assert "uint8 _interrupted_hrv = cpu->host_return_valid;" in source
    assert "cpu->host_return_valid = 0;" in source
    assert "cpu_interrupt_context_enter();" in source
    assert "cpu_interrupt_context_leave();" in source
    assert "cpu->host_return_valid = _interrupted_hrv;" in source


def test_hle_override_covers_all_mx_modes_at_lorom_mirror(tmp_path):
    rom_path, cfg_dir, out_dir = _fixture(tmp_path, target_opcode=0xEA)
    rom = bytearray(rom_path.read_bytes())
    # The only analyzed transfer reaches the LoROM execution mirror in
    # M1X1. At runtime a host scheduler can re-enter the same architectural
    # boundary with any P.M/P.X combination; every exact slot must still use
    # the HLE override rather than interpreting the replaced ROM loop.
    rom[0:4] = bytes([0x5C, 0x99, 0x80, 0x80])  # JML $80:8099
    rom[0x99:0x9B] = bytes([0x80, 0xFE])         # non-returning ROM loop
    rom_path.write_bytes(rom)
    (cfg_dir / "bank00.cfg").write_text(
        "bank = 00\n"
        "func I_RESET 8000 end:8004 entry_mx:1,1\n"
        "func SchedulerLoop 8099 end:809B entry_mx:1,1\n"
        "hle_func 8099 HleSchedulerReturn\n",
        encoding="utf-8")

    result = _run(rom_path, cfg_dir, out_dir)

    assert result.returncode == 0, result.stdout + result.stderr
    assert "exact AOT variants, -" not in result.stdout
    source = (out_dir / "bank80_v2.c").read_text(encoding="utf-8")
    dispatch = (out_dir / "dispatch_v2.c").read_text(encoding="utf-8")
    for m in (0, 1):
        for x in (0, 1):
            name = f"bank_80_8099_M{m}X{x}"
            assert f"RecompReturn {name}(CpuState *cpu)" in source
            assert "RecompReturn _r = HleSchedulerReturn(cpu);" in source
            assert name in dispatch
    assert source.count(
        "cpu_take_tailcall_return_context(NULL, NULL);") == 4
    assert "0x808099u, { NULL" not in dispatch


def test_lle_only_declared_sibling_remains_an_emission_boundary(tmp_path):
    rom_path, cfg_dir, out_dir = _fixture(tmp_path, target_opcode=0x00)
    rom = bytearray(rom_path.read_bytes())
    rom[0:3] = bytes([0x4C, 0x10, 0x80])  # JMP $8010
    rom_path.write_bytes(rom)
    (cfg_dir / "bank00.cfg").write_text(
        "bank = 00\n"
        "func I_RESET 8000 entry_mx:1,1\n"
        "func Poison 8010 entry_mx:1,1\n",
        encoding="utf-8")

    result = _run(rom_path, cfg_dir, out_dir)
    assert result.returncode == 0, result.stdout + result.stderr

    source = (out_dir / "bank00_v2.c").read_text(encoding="utf-8")
    manifest = json.loads(
        (out_dir / "program_manifest.json").read_text(encoding="utf-8"))
    assert manifest["nodes"]["008010:M1X1"]["disposition"] == "lle_only"
    assert "RecompReturn Poison_M1X1" not in source
    assert "interp_tier_dispatch_tail(cpu, 0x008010u" in source
    assert "tail-call past end: missing exact M1X1 body" in source


def test_identical_manifest_reuses_bank_without_changing_mtime(tmp_path):
    rom_path, cfg_dir, out_dir = _fixture(tmp_path, target_opcode=0xEA)
    first = _run(rom_path, cfg_dir, out_dir)
    assert first.returncode == 0, first.stdout + first.stderr
    bank_path = out_dir / "bank00_v2.c"
    first_mtime = bank_path.stat().st_mtime_ns

    second = _run(rom_path, cfg_dir, out_dir)
    assert second.returncode == 0, second.stdout + second.stderr
    assert "0 bank(s) emitted, 1 reused" in second.stdout
    assert "reused verified published output" in second.stdout
    assert bank_path.stat().st_mtime_ns == first_mtime


def test_incremental_cache_rejects_modified_generated_output(tmp_path):
    rom_path, cfg_dir, out_dir = _fixture(tmp_path, target_opcode=0xEA)
    first = _run(rom_path, cfg_dir, out_dir)
    assert first.returncode == 0, first.stdout + first.stderr
    bank_path = out_dir / "bank00_v2.c"
    original = bank_path.read_bytes()
    bank_path.write_text("tampered\n", encoding="utf-8")

    second = _run(rom_path, cfg_dir, out_dir)

    assert second.returncode == 0, second.stdout + second.stderr
    assert "reused verified published output" not in second.stdout
    assert bank_path.read_bytes() == original


def test_incremental_cache_rejects_changed_rom(tmp_path):
    rom_path, cfg_dir, out_dir = _fixture(tmp_path, target_opcode=0xEA)
    first = _run(rom_path, cfg_dir, out_dir)
    assert first.returncode == 0, first.stdout + first.stderr
    rom = bytearray(rom_path.read_bytes())
    rom[0x10] = 0x18  # CLC instead of NOP; same boundaries, different output.
    rom_path.write_bytes(rom)

    second = _run(rom_path, cfg_dir, out_dir)

    assert second.returncode == 0, second.stdout + second.stderr
    assert "reused verified published output" not in second.stdout


def test_atomic_publish_removes_legacy_prefixed_generated_units(tmp_path):
    rom_path, cfg_dir, out_dir = _fixture(tmp_path, target_opcode=0xEA)
    out_dir.mkdir()
    legacy = out_dir / "zelda_00_v2.c"
    legacy.write_text("legacy output\n", encoding="utf-8")

    result = _run(rom_path, cfg_dir, out_dir)

    assert result.returncode == 0, result.stdout + result.stderr
    assert not legacy.exists()
    assert (out_dir / "bank00_v2.c").exists()


def test_host_call_roots_are_inferred_from_handwritten_source(tmp_path):
    cfg = tmp_path / "bank00.cfg"
    cfg.write_text(
        "bank = 00\n"
        "func HostAlias 8123 entry_mx:1,1\n"
        "func ExactAlias 8456 entry_mx:0,0\n",
        encoding="utf-8")
    parsed = [(0, cfg, load_bank_cfg(str(cfg)))]
    source_dir = tmp_path / "src"
    source_dir.mkdir()
    (source_dir / "host.c").write_text(
        "void f(void) { HostAlias(&g_cpu); ExactAlias_M0X1(&g_cpu); }\n",
        encoding="utf-8")

    roots = discover_host_roots(parsed, (source_dir,))
    assert {
        VariantKey(0x008123, m, x)
        for m in (0, 1) for x in (0, 1)
    }.issubset(roots)
    assert VariantKey(0x008456, 0, 1) in roots
    assert VariantKey(0x008456, 1, 1) not in roots


def test_constant_runtime_dispatch_targets_are_host_roots(tmp_path):
    cfg = tmp_path / "bank00.cfg"
    cfg.write_text(
        "bank = 00\nfunc DynamicTarget 8123 entry_mx:1,1\n",
        encoding="utf-8")
    parsed = [(0, cfg, load_bank_cfg(str(cfg)))]
    source = tmp_path / "host.c"
    source.write_text(
        "void f(CpuState *cpu) {\n"
        "  cpu_dispatch_call_pc(cpu, 0x008123u, 0xFFFFFFu);\n"
        "}\n",
        encoding="utf-8")

    roots = discover_host_roots(parsed, (source,))

    assert {key for key in roots if key.pc24 == 0x008123} == {
        VariantKey(0x008123, m, x)
        for m in (0, 1) for x in (0, 1)
    }


def test_host_alias_dispatches_live_mx_and_missing_exact_slot_to_lle(tmp_path):
    rom_path, cfg_dir, out_dir = _fixture(tmp_path, target_opcode=0xEA)
    rom = bytearray(rom_path.read_bytes())
    # M1: LDA #$01; RTS. M0: LDA #$6001; BRK. The M0 entries therefore
    # remain authoritative LLE while both X widths of M1 can be AOT.
    rom[0:4] = bytes([0xA9, 0x01, 0x60, 0x00])
    rom_path.write_bytes(rom)
    source_dir = tmp_path / "src"
    source_dir.mkdir()
    (source_dir / "host.c").write_text(
        "void f(void) { I_RESET(&g_cpu); }\n", encoding="utf-8")
    result = subprocess.run([
        sys.executable, str(REPO / "tools" / "v2_emit.py"),
        "--rom", str(rom_path), "--cfg-dir", str(cfg_dir),
        "--out-dir", str(out_dir), "--source-root", str(source_dir),
    ], text=True, capture_output=True)
    assert result.returncode == 0, result.stdout + result.stderr

    source = (out_dir / "bank00_v2.c").read_text(encoding="utf-8")
    assert "switch (((cpu->m_flag & 1) << 1)" in source
    assert "case 3: _r = I_RESET_M1X1(cpu);" in source
    # Wrong-width reset decodes are structural and intentionally remain LLE.
    assert "interp_tier_dispatch(cpu, 0x008000u)" in source


def _function_def_counts(source: str) -> Counter:
    return Counter(re.findall(
        r'^RecompReturn\s+([A-Za-z_]\w*)\(CpuState \*cpu\) \{',
        source, re.MULTILINE))


def test_cross_bank_name_promoted_when_unclaimed(tmp_path):
    """A cross-bank `name <addr> <friendly>` decl (the address's bank
    differs from the declaring cfg's own bank -- cfg_loader only
    auto-promotes IN-bank decls into cfg.entries, see
    load_bank_cfg's "Auto-promote in-bank name decls" comment) still
    earns its friendly emitted name when nothing else claims it,
    restoring v1/v2_regen's cross-bank labeling for the manifest-driven
    emitter."""
    rom = bytearray([0xFF] * 0x8000)
    rom[0:7] = bytes([0x20, 0x10, 0x80, 0x20, 0x00, 0x81, 0x60])  # I_RESET
    rom[0x10] = 0xEA
    rom[0x11] = 0x60  # $8010: NOP; RTS
    rom[0x100] = 0xEA
    rom[0x101] = 0x60  # $8100: NOP; RTS
    rom[0x7FFC:0x7FFE] = bytes([0x00, 0x80])
    rom[0x7FEA:0x7FEC] = bytes([0xFF, 0xFF])
    rom[0x7FEE:0x7FF0] = bytes([0xFF, 0xFF])
    rom_path = tmp_path / "game.sfc"
    rom_path.write_bytes(rom)
    cfg_dir = tmp_path / "recomp"
    cfg_dir.mkdir()
    (cfg_dir / "bank00.cfg").write_text(
        "bank = 00\nfunc I_RESET 8000 end:8007 entry_mx:1,1\n",
        encoding="utf-8")
    # Declared in bank01's cfg but the address is physically in bank 00 --
    # a cross-bank alias, same shape as a caller bank documenting a callee
    # it JSLs into.
    (cfg_dir / "bank01.cfg").write_text(
        "bank = 01\nname 008100 UniqueCrossBankName\n", encoding="utf-8")
    out_dir = tmp_path / "gen"

    result = subprocess.run([
        sys.executable, str(REPO / "tools" / "v2_emit.py"),
        "--rom", str(rom_path), "--cfg-dir", str(cfg_dir),
        "--out-dir", str(out_dir), "--no-host-root-scan",
    ], text=True, capture_output=True)
    assert result.returncode == 0, result.stdout + result.stderr

    source = (out_dir / "bank00_v2.c").read_text(encoding="utf-8")
    counts = _function_def_counts(source)
    assert counts["UniqueCrossBankName_M1X1"] == 1
    assert all(n == 1 for n in counts.values()), counts


def test_cross_bank_name_collision_falls_back_to_synthetic_name(tmp_path):
    """Two DISTINCT PCs racing for the same friendly name -- one via an
    in-bank `func`, one via a cross-bank `name` decl in another bank's
    cfg -- must never both emit under the same C symbol (MSVC C2084
    same-TU / LNK2005 cross-TU). This is the exact shape of SMW's
    LoadStripeImage regression: `func LoadStripeImage 85d2` in
    bank00.cfg plus bank0c.cfg's `name 0084C8 LoadStripeImage` aliasing
    a distinct wrapper PC. The first claimant (bank/decl order) keeps
    the friendly name; the later, distinct PC falls back to the
    emitter's synthetic bank_<BB>_<AAAA> name instead of colliding."""
    rom = bytearray([0xFF] * 0x8000)
    rom[0:7] = bytes([0x20, 0x10, 0x80, 0x20, 0x00, 0x81, 0x60])  # I_RESET
    rom[0x10] = 0xEA
    rom[0x11] = 0x60  # $8010: NOP; RTS (claims "Foo" via cfg `func`)
    rom[0x100] = 0xEA
    rom[0x101] = 0x60  # $8100: NOP; RTS (collides via cross-bank `name`)
    rom[0x7FFC:0x7FFE] = bytes([0x00, 0x80])
    rom[0x7FEA:0x7FEC] = bytes([0xFF, 0xFF])
    rom[0x7FEE:0x7FF0] = bytes([0xFF, 0xFF])
    rom_path = tmp_path / "game.sfc"
    rom_path.write_bytes(rom)
    cfg_dir = tmp_path / "recomp"
    cfg_dir.mkdir()
    (cfg_dir / "bank00.cfg").write_text(
        "bank = 00\n"
        "func I_RESET 8000 end:8007 entry_mx:1,1\n"
        "func Foo 8010\n",
        encoding="utf-8")
    (cfg_dir / "bank01.cfg").write_text(
        "bank = 01\nname 008100 Foo\n", encoding="utf-8")
    out_dir = tmp_path / "gen"

    result = subprocess.run([
        sys.executable, str(REPO / "tools" / "v2_emit.py"),
        "--rom", str(rom_path), "--cfg-dir", str(cfg_dir),
        "--out-dir", str(out_dir), "--no-host-root-scan",
    ], text=True, capture_output=True)
    assert result.returncode == 0, result.stdout + result.stderr

    source = (out_dir / "bank00_v2.c").read_text(encoding="utf-8")
    counts = _function_def_counts(source)
    # No symbol is defined more than once -- the actual build-breaking bug.
    assert all(n == 1 for n in counts.values()), \
        f"duplicate symbol definition(s): {counts}"
    assert counts["Foo_M1X1"] == 1
    assert counts["bank_00_8100_M1X1"] == 1
