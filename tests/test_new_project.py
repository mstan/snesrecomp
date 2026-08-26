"""End-to-end checks for the new-project scaffolder.

Runs tools/new_project/setup_project.sh against a synthetic, redistributable
SNES image — the same fixture idea tools/smoke_cli_package.py uses, so no real
ROM is needed and nothing copyrighted enters the test tree.

Runs with --no-submodules, so it needs no network and no toolchain and stays
cheap. What it pins is the part that silently rots — the layout, the token
substitution, and the promises the generated files make. Cloning and building
are exercised by hand against a real framework checkout.
"""

from __future__ import annotations

import json
import pathlib
import shutil
import subprocess
import tempfile

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
SETUP = REPO_ROOT / "tools" / "new_project" / "setup_project.sh"
PROBE = REPO_ROOT / "tools" / "new_project" / "probe_rom.py"


def _fixture_rom(path: pathlib.Path) -> None:
    """A 32 KiB LoROM image with a well-formed cartridge header.

    Synthetic and redistributable: 0x60 (RTS) at the reset target, vectors
    pointing at it, and a header whose checksum/complement agree so the probe
    exercises its valid-image path rather than its warning path.
    """
    rom = bytearray([0xFF] * 0x8000)
    rom[0] = 0x60                              # RTS at $00:8000
    base = 0x7FC0
    title = b"FIXTURE QUEST        "           # exactly 21 bytes
    rom[base:base + 21] = title
    rom[base + 0x15] = 0x20                    # LoROM, slow
    rom[base + 0x16] = 0x00                    # ROM only, no coprocessor
    rom[base + 0x17] = 0x05                    # 32 KiB
    rom[base + 0x18] = 0x00                    # no SRAM
    rom[base + 0x19] = 0x01                    # North America
    rom[base + 0x1C:base + 0x1E] = b"\xFF\xFF"  # complement
    rom[base + 0x1E:base + 0x20] = b"\x00\x00"  # checksum
    for offset in (0x0A, 0x0E, 0x1C):          # NMI / IRQ / RESET
        rom[0x7FE0 + offset:0x7FE0 + offset + 2] = bytes([0x00, 0x80])
    path.write_bytes(bytes(rom))


def _scaffold(tmp: pathlib.Path, *extra: str) -> pathlib.Path:
    rom = tmp / "fixture.sfc"
    _fixture_rom(rom)
    result = subprocess.run(
        ["sh", str(SETUP), "--rom", str(rom), "--dir", str(tmp),
         "--name", "Fixture Quest", "--yes", "--no-submodules", *extra],
        cwd=REPO_ROOT, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    assert result.returncode == 0, f"scaffold failed:\n{result.stdout}"
    project = tmp / "FixtureQuestSNESRecomp"
    assert project.is_dir(), f"scaffold produced no project:\n{result.stdout}"
    return project


def test_probe_reads_the_cartridge_header():
    with tempfile.TemporaryDirectory() as directory:
        tmp = pathlib.Path(directory)
        rom = tmp / "fixture.sfc"
        _fixture_rom(rom)
        out = tmp / "probe.json"
        subprocess.run(
            ["python3", str(PROBE), str(rom), "--json-out", str(out), "--quiet"],
            check=True, cwd=REPO_ROOT)
        info = json.loads(out.read_text())

        assert info["mapping"] == "lorom", info["mapping"]
        assert info["region"] == "USA", info["region"]
        assert info["coprocessor"] == "none", info["coprocessor"]
        assert info["header_title"] == "FIXTURE QUEST", info["header_title"]
        assert info["reset_vector"] == "8000", info["reset_vector"]
        assert info["checksum_valid"] is True
        assert info["project_name"] == "FixtureQuestSNESRecomp", info["project_name"]
        assert len(info["sha256"]) == 64


def test_probe_flags_an_unverifiable_header():
    """A bad checksum has to be reported, not silently accepted: it usually
    means an over-dump or a patched image, and generating from one produces
    confusing divergence much later."""
    with tempfile.TemporaryDirectory() as directory:
        tmp = pathlib.Path(directory)
        rom = tmp / "fixture.sfc"
        _fixture_rom(rom)
        raw = bytearray(rom.read_bytes())
        raw[0x7FC0 + 0x1E] = 0x12          # break checksum/complement agreement
        rom.write_bytes(bytes(raw))
        out = tmp / "probe.json"
        subprocess.run(
            ["python3", str(PROBE), str(rom), "--json-out", str(out), "--quiet"],
            check=True, cwd=REPO_ROOT)
        assert json.loads(out.read_text())["checksum_valid"] is False


def test_scaffold_writes_the_expected_layout():
    with tempfile.TemporaryDirectory() as directory:
        project = _scaffold(pathlib.Path(directory))
        expected = [
            "CMakeLists.txt", "VERSION", ".gitignore", "README.md",
            "recomp/bank00.cfg", "recomp/symbols.toml", "recomp/README.md",
            "src/main.c", "src/game_rtl.c", "src/game_rtl.h",
            "src/host_contract.c", "src/variables.h", "src/gen_stubs.c",
            "src/codegen_setup.c", "src/codegen_setup.h",
            "tools/regen.sh", "scripts/package_release.sh",
        ]
        missing = [name for name in expected if not (project / name).is_file()]
        assert not missing, f"scaffold omitted: {missing}"
        for script in ("tools/regen.sh", "scripts/package_release.sh"):
            mode = (project / script).stat().st_mode
            assert mode & 0o111, f"{script} is not executable"


def test_scaffold_leaves_no_unfilled_tokens():
    """An unsubstituted @TOKEN@ in a CMakeLists or a shell script fails much
    later and much more confusingly than it would here."""
    with tempfile.TemporaryDirectory() as directory:
        project = _scaffold(pathlib.Path(directory))
        offenders = []
        for path in project.rglob("*"):
            if not path.is_file() or ".git" in path.parts or "snesrecomp" in path.parts:
                continue
            try:
                text = path.read_text(encoding="utf-8")
            except (UnicodeDecodeError, OSError):
                continue
            if "@" in text:
                import re
                for match in re.findall(r"@[A-Z0-9_]+@", text):
                    offenders.append(f"{path.relative_to(project)}: {match}")
        assert not offenders, f"unfilled tokens: {offenders}"


def test_scaffold_carries_rom_identity_into_the_pipeline():
    """The digests in regen.sh, codegen_setup.c and the README must be the
    same ones — they are what stops a mismatched revision generating quietly
    wrong output."""
    with tempfile.TemporaryDirectory() as directory:
        tmp = pathlib.Path(directory)
        project = _scaffold(tmp)
        import hashlib
        import zlib
        raw = (tmp / "fixture.sfc").read_bytes()
        crc = "%08x" % (zlib.crc32(raw) & 0xFFFFFFFF)
        sha = hashlib.sha256(raw).hexdigest()

        for name in ("tools/regen.sh", "src/codegen_setup.c", "README.md"):
            text = (project / name).read_text(encoding="utf-8")
            assert crc in text, f"{name} is missing the ROM CRC32"
            assert sha in text, f"{name} is missing the ROM SHA-256"


def test_scaffold_never_stages_the_rom():
    """Licensing is not a matter of remembering: the ROM must not be copied
    in, and .gitignore must block it if someone does."""
    with tempfile.TemporaryDirectory() as directory:
        project = _scaffold(pathlib.Path(directory))
        roms = [p for p in project.rglob("*")
                if p.suffix.lower() in (".sfc", ".smc")]
        assert not roms, f"scaffold copied ROM data: {roms}"
        ignore = (project / ".gitignore").read_text(encoding="utf-8")
        for pattern in ("*.sfc", "*.smc", "/src/gen/"):
            assert pattern in ignore, f".gitignore is missing {pattern}"


def test_multitap_options_reach_the_build():
    """--players above two has to configure a tap, or the extra seats are
    unreachable and the project quietly builds a two-player game."""
    with tempfile.TemporaryDirectory() as directory:
        project = _scaffold(pathlib.Path(directory), "--players", "5")
        cmake = (project / "CMakeLists.txt").read_text(encoding="utf-8")
        assert "SNES_MULTITAP=port2" in cmake, cmake[-400:]
        readme = (project / "README.md").read_text(encoding="utf-8")
        assert "Up to 5 players" in readme

    with tempfile.TemporaryDirectory() as directory:
        project = _scaffold(pathlib.Path(directory), "--players", "8")
        cmake = (project / "CMakeLists.txt").read_text(encoding="utf-8")
        assert "SNES_MULTITAP=both" in cmake, cmake[-400:]


def test_rollback_option_reaches_the_build():
    with tempfile.TemporaryDirectory() as directory:
        project = _scaffold(pathlib.Path(directory), "--rollback")
        cmake = (project / "CMakeLists.txt").read_text(encoding="utf-8")
        assert "snesrecomp_enable_recomp_net" in cmake
        assert "snesrecomp_enable_rollback" in cmake

    with tempfile.TemporaryDirectory() as directory:
        project = _scaffold(pathlib.Path(directory))
        cmake = (project / "CMakeLists.txt").read_text(encoding="utf-8")
        assert "snesrecomp_enable_rollback" not in cmake
        assert "Netplay is not built" in cmake


def test_unreachable_framework_remote_creates_nothing():
    """The failure that motivated the preflight: a framework URL that resolves
    to nothing used to fail half way through `git submodule add` and leave a
    partial project directory behind, which the next run then refused to
    overwrite. Check before creating, and create nothing on failure."""
    with tempfile.TemporaryDirectory() as directory:
        tmp = pathlib.Path(directory)
        rom = tmp / "fixture.sfc"
        _fixture_rom(rom)
        result = subprocess.run(
            ["sh", str(SETUP), "--rom", str(rom), "--dir", str(tmp),
             "--name", "Fixture Quest", "--yes", "--snesrecomp-url",
             "https://github.invalid/does-not/exist.git"],
            cwd=REPO_ROOT, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        assert result.returncode != 0, result.stdout
        assert "cannot reach" in result.stdout, result.stdout
        assert "Nothing has been created" in result.stdout, result.stdout
        assert not (tmp / "FixtureQuestSNESRecomp").exists(), \
            "a partial project was left behind"


def test_framework_url_defaults_to_this_checkout():
    """The URL is read from the checkout's own origin rather than hard-coded,
    so it cannot drift from where snesrecomp actually lives."""
    text = SETUP.read_text(encoding="utf-8")
    assert "remote get-url origin" in text
    assert "TechnicallyComputers/snesrecomp" not in text, \
        "the framework URL is hard-coded to the wrong org again"


def test_framework_capability_gaps_are_checked_not_discovered():
    """Reachable is not capable. A framework ref pinned before a feature
    exists cannot provide it, and the scaffolder has to say so in its own
    words rather than letting regen.sh fail later with an argparse error that
    names nothing useful."""
    text = SETUP.read_text(encoding="utf-8")
    # generate, netplay and rollback each get checked against the pinned ref
    assert "snesrecomp_cli.py generate --help" in text
    assert "lib/retcomm-rbengine/CMakeLists.txt" in text
    assert "lib/recomp-net/CMakeLists.txt" in text
    # a gap disables the doomed step instead of running it
    assert "DO_GENERATE=0" in text
    # and the closing hints must not hand back a command that just failed
    assert "fix that first, then:" in text


def test_generate_without_submodules_says_what_to_do():
    """--no-submodules leaves no framework, so generating is impossible. The
    message has to name the fix, not just fail."""
    with tempfile.TemporaryDirectory() as directory:
        tmp = pathlib.Path(directory)
        rom = tmp / "fixture.sfc"
        _fixture_rom(rom)
        result = subprocess.run(
            ["sh", str(SETUP), "--rom", str(rom), "--dir", str(tmp),
             "--name", "Fixture Quest", "--yes", "--no-submodules",
             "--generate"],
            cwd=REPO_ROOT, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        assert result.returncode == 0, result.stdout
        assert "submodule update --init" in result.stdout, result.stdout


def test_github_creation_is_opt_in():
    """Nothing assumes a GitHub repo. Without --create-github there must be no
    remote, no push, and no gh call — just a complete local repository."""
    with tempfile.TemporaryDirectory() as directory:
        tmp = pathlib.Path(directory)
        project = _scaffold(tmp)
        remotes = subprocess.run(
            ["git", "-C", str(project), "remote"],
            text=True, stdout=subprocess.PIPE).stdout.strip()
        assert remotes == "", f"unexpected remote: {remotes!r}"
        log = subprocess.run(
            ["git", "-C", str(project), "log", "--oneline"],
            text=True, stdout=subprocess.PIPE).stdout
        assert "scaffold" in log, log


def test_scaffold_refuses_to_overwrite():
    with tempfile.TemporaryDirectory() as directory:
        tmp = pathlib.Path(directory)
        _scaffold(tmp)
        rom = tmp / "fixture.sfc"
        result = subprocess.run(
            ["sh", str(SETUP), "--rom", str(rom), "--dir", str(tmp),
             "--name", "Fixture Quest", "--yes", "--no-submodules"],
            cwd=REPO_ROOT, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        assert result.returncode != 0, "second run should refuse"
        assert "already exists" in result.stdout, result.stdout
