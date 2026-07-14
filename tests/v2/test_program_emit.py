import json
import pathlib
import subprocess
import sys

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

    assert "I_RESET_M1X1" in source
    assert "bank_00_8010_M1X1" not in source
    assert "interp_tier_run_call_frame(cpu, 0x008010u" in source
    assert "0x008010u, { NULL, NULL, NULL, NULL }" in dispatch
    assert manifest["nodes"]["008010:M1X1"]["disposition"] == "lle_only"


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
    assert "interp_tier_dispatch_balanced(cpu, 0x008010u" in source
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
    assert bank_path.stat().st_mtime_ns == first_mtime


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
