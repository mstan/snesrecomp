import json
import pathlib
import subprocess
import sys
import tempfile


REPO = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "recompiler"))

from v2.program_analysis import VariantKey  # noqa: E402
from v2.program_emit import discover_profile_roots  # noqa: E402


def _fixture(tmp_path):
    rom = bytearray([0xFF] * 0x8000)
    rom[0] = 0x60  # reset: RTS
    rom[0x10:0x12] = bytes([0xEA, 0x60])  # profile target: NOP; RTS
    rom[0x7FFC:0x7FFE] = bytes([0x00, 0x80])
    rom_path = tmp_path / "game.sfc"
    rom_path.write_bytes(rom)
    cfg_dir = tmp_path / "recomp"
    cfg_dir.mkdir()
    (cfg_dir / "bank00.cfg").write_text(
        "bank = 00\n"
        "func I_RESET 8000 end:8001 entry_mx:1,1\n"
        "func Observed 8010 end:8012 entry_mx:1,1\n",
        encoding="utf-8")
    return rom_path, cfg_dir, tmp_path / "gen"


def test_clean_profile_targets_are_optional_aot_roots():
    with tempfile.TemporaryDirectory() as temp:
        profile = pathlib.Path(temp) / "tier2_coverage.json"
        profile.write_text(json.dumps({
            "schema": "snesrecomp tier2 coverage v1",
            "discoveries": [
                {"target_pc24": "0x008123", "entry_mx": "M1X0",
                 "site_kind": "call_gap", "clean_hits": 3, "bail_hits": 0},
                {"target_pc24": "0x008456", "entry_mx": "M0X1",
                 "clean_hits": 2, "bail_hits": 1},
                {"target_pc24": "0x008999", "entry_mx": "unknown",
                 "clean_hits": 1, "bail_hits": 0},
                {"target_pc24": "0x008ABC", "entry_mx": "M0X0",
                 "site_kind": "goto_gap", "clean_hits": 5,
                 "bail_hits": 0},
            ],
        }), encoding="utf-8")

        assert discover_profile_roots((profile,)) == (
            VariantKey(0x008123, 1, 0),)


def test_declared_profile_target_is_safe_even_when_landing_is_not_a_call():
    with tempfile.TemporaryDirectory() as temp:
        profile = pathlib.Path(temp) / "tier2_coverage.json"
        profile.write_text(json.dumps({
            "schema": "snesrecomp tier2 coverage v1",
            "discoveries": [
                {"target_pc24": "0x808ABC", "entry_mx": "M0X0",
                 "site_kind": "indirect_dispatch", "clean_hits": 5,
                 "bail_hits": 0},
            ],
        }), encoding="utf-8")

        # A LoROM mirror of a declared boundary carries the same function ABI.
        assert discover_profile_roots((profile,), (0x008ABC,)) == (
            VariantKey(0x808ABC, 0, 0),)


def test_profile_manifest_materializes_observed_exact_variant():
    with tempfile.TemporaryDirectory() as temp:
        tmp_path = pathlib.Path(temp)
        rom_path, cfg_dir, out_dir = _fixture(tmp_path)
        profile = tmp_path / "tier2_coverage.json"
        profile.write_text(json.dumps({
            "schema": "snesrecomp tier2 coverage v1",
            "discoveries": [
                {"target_pc24": "0x008010", "entry_mx": "M1X1",
                 "clean_hits": 4, "bail_hits": 0},
            ],
        }), encoding="utf-8")

        result = subprocess.run([
            sys.executable, str(REPO / "tools" / "v2_emit.py"),
            "--rom", str(rom_path), "--cfg-dir", str(cfg_dir),
            "--out-dir", str(out_dir), "--no-host-root-scan",
            "--profile-manifest", str(profile),
        ], text=True, capture_output=True)

        assert result.returncode == 0, result.stdout + result.stderr
        manifest = json.loads(
            (out_dir / "program_manifest.json").read_text(encoding="utf-8"))
        assert {root["pc24"] for root in manifest["roots"]} >= {
            0x008000, 0x008010}
        assert manifest["nodes"]["008010:M1X1"]["disposition"] == \
            "aot_eligible"
        assert "Observed_M1X1" in (out_dir / "bank00_v2.c").read_text(
            encoding="utf-8")
