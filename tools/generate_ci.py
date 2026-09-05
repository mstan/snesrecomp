#!/usr/bin/env python3
"""generate_ci.py -- write the release workflow into an EXISTING SNES project.

New projects get .github/workflows/release.yml from the scaffolder. Projects
that predate it, or that were cut before the template gained a step, have to
get it from somewhere, and that somewhere is here: the same template the
scaffolder fills (tools/new_project/templates/release.yml.in), filled with
values read out of the project itself rather than re-typed.

    python3 snesrecomp/tools/generate_ci.py                # cwd is the project
    python3 snesrecomp/tools/generate_ci.py ~/src/MyGameSNESRecomp
    python3 snesrecomp/tools/generate_ci.py --check        # stale? (exit 1)
    python3 snesrecomp/tools/generate_ci.py --force        # overwrite

Where the values come from, in order:
  PROJECT_NAME  CMakeLists.txt  project(<name> ...)
  DISPLAY_NAME  rom_identity.txt display_name, else src/codegen_setup.c
                .display_name = "...", else --display-name
  ZIP_PREFIX    scripts/package_release.sh --zip-prefix <x>  (the packager's
                prefix, so the zips CI uploads are the zips it built), else
                a slug of the display name, else --zip-prefix

The template is taken from the project's own snesrecomp submodule when it has
one, so the workflow matches the framework the project actually pins; the
checkout this script runs from is the fallback.

An existing, filled release.yml is not overwritten silently. Its step names
are compared with the template's: a step the template defines and the file
lacks means the file predates a framework change and is reported as stale.
--force overwrites (a customised workflow is replaced wholesale -- diff it).
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
FRAMEWORK_ROOT = HERE.parent
sys.path.insert(0, str(HERE / "new_project"))

WORKFLOW_REL = pathlib.Path(".github") / "workflows" / "release.yml"
TEMPLATE_REL = pathlib.Path("tools") / "new_project" / "templates" / "release.yml.in"


def _read(path: pathlib.Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def project_name_from_cmake(root: pathlib.Path) -> str:
    m = re.search(r"^\s*project\(\s*([A-Za-z0-9_]+)", _read(root / "CMakeLists.txt"), re.M)
    return m.group(1) if m else ""


def display_name_from_identity(root: pathlib.Path) -> str:
    """rom_identity.txt is the one place a scaffolded project keeps it."""
    identity = root / "rom_identity.txt"
    if not identity.is_file():
        return ""
    try:
        from rom_identity import parse  # tools/rom_identity.py
    except ImportError:
        sys.path.insert(0, str(HERE))
        from rom_identity import parse
    return parse(identity).get("display_name", "")


def display_name_from_codegen_setup(root: pathlib.Path) -> str:
    """Older projects carry it in the codegen identity struct. A macro
    (MetalWarriors' MW_DISPLAY) is not a value; only a literal counts."""
    for name in ("src/codegen_setup.c", "codegen_setup.c"):
        m = re.search(r'\.display_name\s*=\s*"([^"]*)"', _read(root / name))
        if m and m.group(1).strip():
            return m.group(1).strip()
    return ""


def zip_prefix_from_packager(root: pathlib.Path) -> str:
    m = re.search(r"--zip-prefix\s+([^\s\\'\"]+)", _read(root / "scripts" / "package_release.sh"))
    return m.group(1) if m else ""


def slug(display: str) -> str:
    from probe_rom import safe_slug  # tools/new_project/probe_rom.py
    return safe_slug(display).lower()


def template_for(root: pathlib.Path) -> pathlib.Path | None:
    candidates = [root / "snesrecomp" / TEMPLATE_REL, FRAMEWORK_ROOT / TEMPLATE_REL]
    for c in candidates:
        if c.is_file():
            return c
    return None


def step_names(text: str) -> list[str]:
    """Step names, by regex over the raw text: the template holds @TOKEN@
    placeholders, which need no interpretation, and this must not depend on
    a yaml module. Names that carry a token cannot be compared literally."""
    names = []
    for line in text.splitlines():
        m = re.match(r"^\s*-\s+name:\s*(.+?)\s*$", line)
        if m:
            names.append(m.group(1).strip().strip("\"'"))
    return names


def steps_missing(installed: str, template: str) -> list[str]:
    have = set(step_names(installed))
    return [n for n in step_names(template) if n not in have and "@" not in n]


def prerequisites(root: pathlib.Path) -> list[str]:
    """What the workflow will reach for at run time. Warnings, not errors:
    the file is still worth writing, and the run names what is missing."""
    gaps = []
    if not (root / "scripts" / "package_release.sh").is_file():
        gaps.append("scripts/package_release.sh -- the build job calls it to make the setup pack")
    if not (root / "VERSION").is_file():
        gaps.append("VERSION -- preflight reads it for the first release's number")
    if not (root / "framework_pins.txt").is_file():
        gaps.append("framework_pins.txt -- preflight can check submodules against it "
                    "(bash snesrecomp/tools/ci/record_pins.sh writes one)")
    if not (root / "rom_identity.txt").is_file():
        gaps.append("rom_identity.txt -- the publish job reads the ROM name and SHA-256 "
                    "from it (display_name / rom_file / expected_sha256, see "
                    "tools/new_project/templates/rom_identity.txt.in)")
    if (root / "snesrecomp").is_dir() and not (root / "snesrecomp" / "tools" / "ci" / "record_pins.sh").is_file():
        gaps.append("snesrecomp/tools/ci/ -- the pinned framework predates setup packs; "
                    "bump the submodule")
    return gaps


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog="\n".join(__doc__.split("\n\n")[1:]))
    ap.add_argument("root", nargs="?", default=".", help="project root (default: cwd)")
    ap.add_argument("--display-name", default="", help="override the title")
    ap.add_argument("--project-name", default="", help="override the CMake target name")
    ap.add_argument("--zip-prefix", default="", help="override the release zip prefix")
    ap.add_argument("--force", action="store_true", help="overwrite an existing release.yml")
    ap.add_argument("--check", action="store_true",
                    help="report whether release.yml is present and current; exit 1 if not")
    ap.add_argument("--dry-run", action="store_true", help="print the workflow, write nothing")
    args = ap.parse_args()

    root = pathlib.Path(args.root).resolve()
    if not (root / "CMakeLists.txt").is_file():
        print(f"generate_ci: {root} has no CMakeLists.txt -- not a project root", file=sys.stderr)
        return 2

    template = template_for(root)
    if template is None:
        print("generate_ci: no release.yml.in found (neither the project's snesrecomp "
              "submodule nor this checkout has tools/new_project/templates/)", file=sys.stderr)
        return 2

    project = args.project_name or project_name_from_cmake(root)
    display = (args.display_name or display_name_from_identity(root)
               or display_name_from_codegen_setup(root))
    zip_prefix = args.zip_prefix or zip_prefix_from_packager(root) or (slug(display) if display else "")
    missing = [k for k, v in (("PROJECT_NAME", project), ("DISPLAY_NAME", display),
                              ("ZIP_PREFIX", zip_prefix)) if not v]
    if missing:
        print(f"generate_ci: could not determine {', '.join(missing)} from {root}.",
              file=sys.stderr)
        print("  Pass --project-name / --display-name / --zip-prefix, or add "
              "rom_identity.txt (display_name) and scripts/package_release.sh "
              "(--zip-prefix) to the project.", file=sys.stderr)
        return 2

    dst = root / WORKFLOW_REL
    template_text = template.read_text(encoding="utf-8")

    if dst.is_file():
        installed = _read(dst)
        stale = steps_missing(installed, template_text)
        unfilled = bool(re.search(r"@[A-Z0-9_]+@", installed))
        if args.check:
            if unfilled:
                print(f"{WORKFLOW_REL}: still has unfilled @TOKEN@ placeholders")
                return 1
            if stale:
                print(f"{WORKFLOW_REL}: stale -- missing step(s): {', '.join(stale)}")
                return 1
            print(f"{WORKFLOW_REL}: present and current")
            return 0
        if not args.force and not args.dry_run:
            if stale:
                print(f"generate_ci: {WORKFLOW_REL} is out of date with the template; "
                      f"missing step(s): {', '.join(stale)}", file=sys.stderr)
                print("  Re-run with --force to replace it (a customised workflow is "
                      "overwritten wholesale -- review the diff first).", file=sys.stderr)
                return 1
            if unfilled:
                print(f"generate_ci: {WORKFLOW_REL} has unfilled placeholders; "
                      "re-run with --force to replace it", file=sys.stderr)
                return 1
            print(f"{WORKFLOW_REL}: already present and current (--force to rewrite)")
            return 0
    elif args.check:
        print(f"{WORKFLOW_REL}: missing")
        return 1

    from fill_tokens import render  # tools/new_project/fill_tokens.py
    values = {"PROJECT_NAME": project, "DISPLAY_NAME": display, "ZIP_PREFIX": zip_prefix}
    rendered, unset = render(template_text, values)
    if unset:
        print(f"generate_ci: template has tokens this script does not know: "
              f"{', '.join(sorted(set(unset)))}", file=sys.stderr)
        return 2

    print(f"template:     {template}")
    print(f"project:      {project}")
    print(f"display name: {display}")
    print(f"zip prefix:   {zip_prefix}")
    for gap in prerequisites(root):
        print(f"warning: missing {gap}", file=sys.stderr)

    if args.dry_run:
        sys.stdout.write(rendered)
        return 0
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text(rendered, encoding="utf-8")
    print(f"wrote {dst}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
