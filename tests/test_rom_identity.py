"""The CMake emitter and the Python emitter must agree, byte for byte.

rom_identity.txt is the single source of truth for a project's ROM digests.
Two things generate snesrecomp_rom_identity.h from it -- CMake at configure
time (runner/runner.cmake) and tools/rom_identity.py for CI, which cannot
configure a project that has no generated sources yet. The data is not
duplicated, but the parser is, so this pins them together: if one learns a
quoting rule or a field the other does not, these fail.
"""

import pathlib
import shutil
import subprocess
import tempfile

import pytest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
EMITTER = REPO_ROOT / "tools" / "rom_identity.py"
CMAKE_MODULE = REPO_ROOT / "runner" / "runner.cmake"

IDENTITY = """\
# a comment, and a blank line follow

display_name    = Fixture Quest
rom_file        = fixture.sfc
expected_crc32  = 6852cf05
expected_sha256 = 4400876a57dc43a85021732727b673def55fd2008c7329ce4fcfc6792ac5ce70
mapping         = lorom
region          = JPN
game_id         = fixture-jp
"""


def _python_header(tmp: pathlib.Path, identity: pathlib.Path) -> str:
    out = tmp / "py"
    subprocess.run(
        ["python3", str(EMITTER), str(identity), "--header", str(out)],
        check=True, capture_output=True, text=True)
    return (out / "snesrecomp_rom_identity.h").read_text(encoding="utf-8")


def _cmake_header(tmp: pathlib.Path, identity: pathlib.Path) -> str:
    """Drive the real CMake function, so this tests the shipped code path."""
    proj = tmp / "cm"
    proj.mkdir()
    (proj / "CMakeLists.txt").write_text(f"""
cmake_minimum_required(VERSION 3.16)
project(identity_probe C)
set(SNESRECOMP_RUNNER_ROOT "{(REPO_ROOT / 'runner').as_posix()}")
include("{CMAKE_MODULE.as_posix()}")
add_library(probe STATIC probe.c)
snesrecomp_rom_identity(probe "{identity.as_posix()}")
""", encoding="utf-8")
    (proj / "probe.c").write_text("int probe(void) { return 0; }\n",
                                  encoding="utf-8")
    r = subprocess.run(["cmake", "-S", str(proj), "-B", str(proj / "b")],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    found = list((proj / "b").rglob("snesrecomp_rom_identity.h"))
    assert found, "CMake did not generate the header"
    return found[0].read_text(encoding="utf-8")


@pytest.mark.skipif(shutil.which("cmake") is None, reason="cmake not present")
def test_emitters_agree():
    with tempfile.TemporaryDirectory() as directory:
        tmp = pathlib.Path(directory)
        identity = tmp / "rom_identity.txt"
        identity.write_text(IDENTITY, encoding="utf-8")

        py = _python_header(tmp, identity)
        cm = _cmake_header(tmp, identity)
        # The source path is embedded in the banner; compare the rest.
        strip = lambda t: [l for l in t.splitlines()
                           if "Generated from" not in l]
        assert strip(py) == strip(cm)


def test_values_reach_the_header():
    with tempfile.TemporaryDirectory() as directory:
        tmp = pathlib.Path(directory)
        identity = tmp / "rom_identity.txt"
        identity.write_text(IDENTITY, encoding="utf-8")
        header = _python_header(tmp, identity)
        assert '"6852cf05"' in header
        assert ('"4400876a57dc43a85021732727b673def55fd2008c7329ce4fcfc6792ac'
                '5ce70"') in header
        assert '"fixture.sfc"' in header
        assert '#define SNESRECOMP_ROM_GAME_ID         "fixture-jp"' in header


def test_missing_field_is_empty_not_absent():
    """A consumer reads "" as 'cannot verify'. The macro must still exist, or
    every host fails to compile against an older project file."""
    with tempfile.TemporaryDirectory() as directory:
        tmp = pathlib.Path(directory)
        identity = tmp / "rom_identity.txt"
        identity.write_text("display_name = Only This\n", encoding="utf-8")
        header = _python_header(tmp, identity)
        assert '#define SNESRECOMP_ROM_EXPECTED_SHA256 ""' in header
        assert '#define SNESRECOMP_ROM_EXPECTED_CRC32  ""' in header


def test_quoted_values_are_unwrapped():
    with tempfile.TemporaryDirectory() as directory:
        tmp = pathlib.Path(directory)
        identity = tmp / "rom_identity.txt"
        identity.write_text('rom_file = "Game (USA).sfc"\n', encoding="utf-8")
        header = _python_header(tmp, identity)
        assert '#define SNESRECOMP_ROM_FILE            "Game (USA).sfc"' in header
