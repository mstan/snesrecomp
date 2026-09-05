"""tools/generate_ci.py writes the same release workflow the scaffolder does,
into a project that already exists -- reading its values from the project
rather than asking for them again -- and knows when the one it finds is stale.
"""

import pathlib
import re
import subprocess
import sys
import tempfile

import pytest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
GENERATE = REPO_ROOT / "tools" / "generate_ci.py"
GENERATE_SH = REPO_ROOT / "tools" / "generate_ci.sh"
WORKFLOW = pathlib.Path(".github") / "workflows" / "release.yml"

sys.path.insert(0, str(REPO_ROOT / "tests"))
from test_new_project import _fixture_rom, SETUP  # noqa: E402


def _scaffold(tmp: pathlib.Path, *extra: str) -> pathlib.Path:
    rom = tmp / "fixture.sfc"
    _fixture_rom(rom)
    result = subprocess.run(
        ["sh", str(SETUP), "--rom", str(rom), "--dir", str(tmp),
         "--name", "Fixture Quest", "--yes", "--no-submodules", *extra],
        cwd=REPO_ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    assert result.returncode == 0, result.stdout
    return tmp / "FixtureQuestSNESRecomp"


def _run(*args: str, cwd=None) -> subprocess.CompletedProcess:
    return subprocess.run(["python3", str(GENERATE), *args], cwd=cwd, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def test_matches_what_the_scaffolder_writes():
    """A project scaffolded with --no-ci, then generate_ci, must end up with
    exactly the workflow a --ci scaffold would have written: same template,
    same values, read back from the project instead of retyped."""
    with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
        without = _scaffold(pathlib.Path(a), "--no-ci")
        with_ci = _scaffold(pathlib.Path(b))
        assert not (without / WORKFLOW).exists()
        r = _run(str(without))
        assert r.returncode == 0, r.stdout + r.stderr
        assert (without / WORKFLOW).read_text() == (with_ci / WORKFLOW).read_text()
        assert "display name: Fixture Quest" in r.stdout
        assert "zip prefix:   fixturequest" in r.stdout
        # A scaffolded project has every prerequisite the workflow reaches for.
        assert "warning: missing" not in r.stderr, r.stderr


def test_current_file_is_left_alone_and_stale_one_is_reported():
    with tempfile.TemporaryDirectory() as d:
        project = _scaffold(pathlib.Path(d))
        wf = project / WORKFLOW
        original = wf.read_text()

        r = _run("--check", cwd=project)
        assert r.returncode == 0 and "present and current" in r.stdout, r.stdout
        r = _run(cwd=project)
        assert r.returncode == 0 and "already present and current" in r.stdout
        assert wf.read_text() == original, "a current file was rewritten"

        # Drop one step: the file now predates the template.
        lines = [l for l in original.splitlines(keepends=True)
                 if "record_pins" not in l and "Record framework pins" not in l]
        stale = "".join(l for l in lines if not l.strip().startswith("- name: Record"))
        wf.write_text(stale)
        r = _run("--check", cwd=project)
        assert r.returncode == 1 and "stale" in r.stdout, r.stdout
        r = _run(cwd=project)
        assert r.returncode == 1 and "out of date" in r.stderr, r.stderr
        assert wf.read_text() == stale, "refused, so must not have written"
        r = _run("--force", cwd=project)
        assert r.returncode == 0, r.stdout + r.stderr
        assert wf.read_text() == original


def test_reads_an_older_layout_and_names_what_the_workflow_will_miss():
    """A project from before rom_identity.txt: title in codegen_setup.c, zip
    prefix in the packager, no submodule (template comes from this checkout).
    It gets a workflow, and a warning for each thing the run will reach for."""
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d) / "OldGameSNESRecomp"
        (root / "src").mkdir(parents=True)
        (root / "scripts").mkdir()
        (root / "CMakeLists.txt").write_text("cmake_minimum_required(VERSION 3.20)\n"
                                             "project(OldGameSNESRecomp C CXX)\n")
        (root / "src" / "codegen_setup.c").write_text(
            'const GameCodegenIdentity kGameCodegenIdentity = {\n'
            '    .display_name   = "Old Game",\n'
            '    .rom_file       = "Old Game (USA).sfc",\n'
            '};\n')
        (root / "scripts" / "package_release.sh").write_text(
            "exec bash snesrecomp/tools/ci/stage_setup_host.sh \\\n"
            "  --zip-prefix oldgame \\\n  --display-name \"Old Game\"\n")
        r = _run(str(root))
        assert r.returncode == 0, r.stdout + r.stderr
        text = (root / WORKFLOW).read_text()
        assert "Old Game" in text and "oldgame" in text
        assert not re.search(r"@[A-Z0-9_]+@", text), "unfilled token"
        for needed in ("VERSION", "framework_pins.txt", "rom_identity.txt"):
            assert f"warning: missing {needed}" in r.stderr, r.stderr


def test_says_what_it_could_not_determine():
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        (root / "CMakeLists.txt").write_text("project(Bare C)\n")
        r = _run(str(root))
        assert r.returncode == 2
        assert "DISPLAY_NAME" in r.stderr and "--display-name" in r.stderr
        r = _run(str(root), "--display-name", "Bare Game")
        assert r.returncode == 0, r.stderr
        assert "baregame" in (root / WORKFLOW).read_text()


@pytest.mark.skipif(sys.platform.startswith("win"), reason="sh wrapper")
def test_sh_wrapper_passes_everything_through():
    with tempfile.TemporaryDirectory() as d:
        project = _scaffold(pathlib.Path(d))
        r = subprocess.run(["sh", str(GENERATE_SH), "--check"], cwd=project, text=True,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        assert r.returncode == 0 and "present and current" in r.stdout, r.stdout
