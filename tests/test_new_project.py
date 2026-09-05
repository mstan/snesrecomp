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

import pytest

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
            "rom_identity.txt",
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
    """rom_identity.txt is the one place the digests live, and every consumer
    must reach them from there — that is what stops a mismatched revision
    generating quietly wrong output."""
    with tempfile.TemporaryDirectory() as directory:
        tmp = pathlib.Path(directory)
        project = _scaffold(tmp)
        import hashlib
        import zlib
        raw = (tmp / "fixture.sfc").read_bytes()
        crc = "%08x" % (zlib.crc32(raw) & 0xFFFFFFFF)
        sha = hashlib.sha256(raw).hexdigest()

        identity = (project / "rom_identity.txt").read_text(encoding="utf-8")
        assert crc in identity, "rom_identity.txt is missing the ROM CRC32"
        assert sha in identity, "rom_identity.txt is missing the ROM SHA-256"

        # The README is documentation and still states them for a reader.
        readme = (project / "README.md").read_text(encoding="utf-8")
        assert crc in readme and sha in readme, "README lost the digests"


def test_digests_are_not_copied_into_code_or_scripts():
    """The point of rom_identity.txt: no consumer may carry its own copy of a
    digest, because a second copy is a second thing to forget on a revision
    bump. regen.sh must PARSE the file, and the build must GENERATE from it."""
    with tempfile.TemporaryDirectory() as directory:
        tmp = pathlib.Path(directory)
        project = _scaffold(tmp)
        import hashlib
        import zlib
        raw = (tmp / "fixture.sfc").read_bytes()
        crc = "%08x" % (zlib.crc32(raw) & 0xFFFFFFFF)
        sha = hashlib.sha256(raw).hexdigest()

        for name in ("tools/regen.sh", "src/main.c", "CMakeLists.txt",
                     "scripts/package_release.sh",
                     ".github/workflows/release.yml"):
            text = (project / name).read_text(encoding="utf-8")
            assert crc not in text, f"{name} hardcodes the CRC32"
            assert sha not in text, f"{name} hardcodes the SHA-256"

        regen = (project / "tools/regen.sh").read_text(encoding="utf-8")
        assert "rom_identity.txt" in regen, "regen.sh does not read the identity"
        cmake = (project / "CMakeLists.txt").read_text(encoding="utf-8")
        assert "snesrecomp_rom_identity" in cmake, "build does not generate it"


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


def test_framework_ref_defaults_to_this_checkouts_branch():
    """A scaffold cut from a checkout has to pin the framework that checkout
    actually has. Hard-coding "main" silently produced projects that could
    not generate or build whenever the work lived on a branch — which is the
    normal state while a feature is in progress."""
    text = SETUP.read_text(encoding="utf-8")
    assert "symbolic-ref --quiet --short HEAD" in text
    # and a branch the remote does not have must still yield a working tree
    assert "FRAMEWORK_REF_UNPUSHED" in text
    # The local commit has to be staged before `submodule update` runs, or it
    # is snapped back to whatever `submodule add` recorded. Anchor on the
    # submodule section: the flag documentation in the header mentions the
    # same command and would otherwise match first.
    section = text[text.index('echo "== Adding submodules =="'):]
    add_at = section.index("git add snesrecomp")
    update_at = section.index("git submodule update --init --recursive")
    assert add_at < update_at, \
        "the gitlink must be staged before submodule update restores it"


def test_ci_workflow_is_manual_only():
    """A run is a release decision made by a person on the Actions page.
    Releases across the port set are driven from the studio bulk tool, and a
    workflow that also fires on every push competes with it: wasted minutes,
    and an ambiguous answer to "did CI pass" when the two disagree."""
    with tempfile.TemporaryDirectory() as directory:
        tmp = pathlib.Path(directory)
        project = _scaffold(tmp, "--ci")
        workflow = project / ".github" / "workflows" / "release.yml"
        assert workflow.is_file(), "CI was requested but no workflow was written"
        text = workflow.read_text(encoding="utf-8")

        body = text[text.index("\non:"):text.index("\njobs:")]
        assert "workflow_dispatch" in body, body
        for trigger in ("push:", "pull_request:", "schedule:"):
            assert trigger not in body, f"{trigger} must not trigger this workflow"

        # Publishing is the default and a checkbox, not a tag ceremony: the
        # workflow chooses the next version and tags the commit itself.
        publish = body[body.index("publish_release:"):]
        publish = publish[:publish.index("\n      version:")]
        assert "default: true" in publish, publish
        assert "version:" in body and "bump:" in body
        assert "git tag -a" in text and "git push origin \"refs/tags/" in text
        assert "make_latest: true" in text


def test_host_template_builds_against_both_sdl_backends():
    """The scaffolded host must compile under SDL2 and SDL3.

    sdl_compat.h defines SDL_ENABLE_OLD_NAMES for SDL3, so the SDL2 spellings
    work on both; the SDL3-only names do not. This shipped broken because the
    development machine had SDL3 and nothing compiled it against SDL2 until
    the generated CI did.
    """
    import re
    template = (REPO_ROOT / "tools" / "new_project" / "templates"
                / "main.c.in").read_text(encoding="utf-8")
    # Scan code only: the comment explaining this rule names the very
    # identifiers it forbids.
    code = re.sub(r"/\*.*?\*/", " ", template, flags=re.S)
    code = re.sub(r"//[^\n]*", " ", code)
    sdl3_only = [name for name in ("SDL_EVENT_QUIT", "SDL_EVENT_KEY_DOWN",
                                   "SDL_EVENT_KEY_UP")
                 if name in code]
    assert not sdl3_only, f"SDL3-only names in the host template: {sdl3_only}"


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


def test_a_bare_argument_is_the_rom():
    """`setup_project.sh game.sfc` -- the ROM is the one thing a new project
    is cut from, so it does not need a flag."""
    with tempfile.TemporaryDirectory() as directory:
        tmp = pathlib.Path(directory)
        rom = tmp / "fixture.sfc"
        _fixture_rom(rom)
        result = subprocess.run(
            ["sh", str(SETUP), str(rom), "--dir", str(tmp),
             "--name", "Fixture Quest", "--yes", "--no-submodules"],
            cwd=REPO_ROOT, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        assert result.returncode == 0, result.stdout
        assert (tmp / "FixtureQuestSNESRecomp" / "rom_identity.txt").is_file()


def test_no_rom_is_an_error_when_nobody_can_be_asked():
    with tempfile.TemporaryDirectory() as directory:
        result = subprocess.run(
            ["sh", str(SETUP), "--dir", directory, "--yes", "--no-submodules"],
            cwd=REPO_ROOT, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        assert result.returncode != 0
        assert "--rom" in result.stdout, result.stdout
        assert not any(pathlib.Path(directory).iterdir()), "created something"


def test_non_interactive_runs_do_not_reach_the_network():
    """Boxart is a download. A --yes run (scripts, CI, this suite) must not
    start one because a default said so; it is asked on a terminal and
    otherwise needs --fetch-boxart."""
    with tempfile.TemporaryDirectory() as directory:
        tmp = pathlib.Path(directory)
        rom = tmp / "fixture.sfc"
        _fixture_rom(rom)
        result = subprocess.run(
            ["sh", str(SETUP), "--rom", str(rom), "--dir", str(tmp),
             "--name", "Fixture Quest", "--yes", "--no-submodules"],
            cwd=REPO_ROOT, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        assert result.returncode == 0, result.stdout
        assert "Fetching boxart" not in result.stdout, result.stdout
        project = tmp / "FixtureQuestSNESRecomp"
        assert not (project / "launcher_assets" / "img" / "boxart.tga").exists()


def _drive_on_a_pty(cmd, answer, timeout=120):
    """Run cmd on a pseudo-terminal, answering each prompt through answer().
    setup_project.sh reads /dev/tty, so a pipe cannot exercise its questions;
    this is the only way to test the flow a person actually sees."""
    import os
    import pty
    import select
    import time

    pid, fd = pty.fork()
    if pid == 0:  # child
        os.execvp(cmd[0], cmd)
    out = b""
    pending = b""
    last = time.time()
    while True:
        ready, _, _ = select.select([fd], [], [], 0.5)
        if ready:
            try:
                chunk = os.read(fd, 4096)
            except OSError:
                break
            if not chunk:
                break
            out += chunk
            pending += chunk
            last = time.time()
            if pending.endswith(b": "):
                prompt = pending.decode(errors="replace").splitlines()[-1]
                os.write(fd, answer(prompt).encode() + b"\n")
                pending = b""
        elif time.time() - last > timeout:
            os.kill(pid, 9)
            break
    _, status = os.waitpid(pid, 0)
    return os.waitstatus_to_exitcode(status), out.decode(errors="replace")


@pytest.mark.skipif(not hasattr(__import__("os"), "fork"), reason="needs fork/pty")
def test_the_interactive_flow_asks_and_listens():
    """No flags at all: the ROM is the first question, every setting after it
    is asked with a probed default, and the answers reach the project."""
    with tempfile.TemporaryDirectory() as directory:
        tmp = pathlib.Path(directory)
        rom = tmp / "fixture.sfc"
        _fixture_rom(rom)

        asked = []

        def answer(prompt):
            asked.append(prompt)
            p = prompt.lower()
            if "path to the rom" in p:
                return str(rom)
            if "max players" in p:
                return "3"
            if "short description" in p:
                return "Driven through a pty"
            if "boxart" in p or "netplay" in p or "generate c" in p:
                return "n"
            return ""   # take the default

        code, out = _drive_on_a_pty(
            ["sh", str(SETUP), "--dir", str(tmp), "--no-submodules"], answer)
        assert code == 0, out

        text = " | ".join(asked)
        for question in ("Path to the ROM", "Display name", "Max players",
                         "Multitap", "boxart", "netplay", "GitHub Actions",
                         "Generate C", "gh?", "Proceed?"):
            assert question in text, f"never asked {question!r}:\n{out}"

        project = tmp / "FixtureQuestSNESRecomp"
        assert project.is_dir(), out
        readme = (project / "README.md").read_text(encoding="utf-8")
        assert "Driven through a pty" in readme
        cmake = (project / "CMakeLists.txt").read_text(encoding="utf-8")
        assert "Seats: 3" in cmake and "port2" in cmake, cmake
        assert not (project / "launcher_assets" / "img" / "boxart.tga").exists()
