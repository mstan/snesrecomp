# New project toolkit

Scaffold a new SNES recomp title end to end: probe the ROM, lay out the repo,
wire the submodules, seed the analysis config, generate C, build, and publish.

```sh
sh tools/new_project/setup_project.sh ~/roms/game.sfc --dir ~/src
```

```powershell
powershell -File tools\new_project\setup_project.ps1 -Rom C:\roms\game.sfc -Dir C:\src
```

On a terminal the ROM is the only argument you need — and with none at all it
is the first question. Everything else is asked, with the probed ROM identity
supplying the defaults. Flags are for scripting: anything passed explicitly
skips its question, and `--yes` (or a non-TTY run) takes every default without
asking, with network toggles (boxart, GitHub) off unless flagged.

`sh setup_project.sh --help` lists every flag. The PowerShell entry point is a
thin launcher: it finds Git for Windows' bash and runs the same script with the
same arguments, so there is one scaffolder to fix. It needs Git for Windows,
Python 3 and CMake on `PATH`; WSL and Git Bash users can run the `.sh` directly.

## What it asks

| Question | Default |
|---|---|
| Display name | the dump filename, cleaned of `(Region)` tags, or the cartridge header when that reads better |
| Max players (1-8) | 2 |
| Multitap port | derived from players; only asked above 2 seats |
| Release zip / CI artifact prefix | slug of the name |
| Description, publisher, year | blank (README metadata) |
| Region | from the cartridge header |
| Include the recomp-ui launcher submodule? | yes |
| Fetch libretro boxart now? (network) | yes on a terminal; off when non-interactive |
| Enable netplay? | no — skipped entirely for a 1-player title |
| Also build rollback? | yes, when netplay is on |
| Add the GitHub Actions workflow? | yes |
| Generate C from the ROM now? | yes |
| Configure and build after generating? | yes, when generating |
| Create a GitHub repo with `gh`? | **no** |
| Owner / repo / visibility | asked only if you said yes |

Nothing assumes a GitHub repository exists. Declining the question skips
creation, the remote, and the push; the project is still a complete local git
repository. The SNES counterpart to
psxrecomp's `tools/new_project_layout/setup_project.sh`, and it follows the
same publish order for the same reason: scaffold + CI → commit → `gh repo
create` (no push) → generate/build → one push. Pushing earlier leaves a second
"initial" commit that collides when the script is re-run.

## Existing projects: the release workflow on its own

A project that predates the scaffolder, or was cut before the CI template
gained a step, gets the same workflow from `tools/generate_ci`:

```sh
sh snesrecomp/tools/generate_ci.sh            # in the project; --check, --force, --dry-run
```

```powershell
powershell -File snesrecomp\tools\generate_ci.ps1
```

It fills `templates/release.yml.in` with values read from the project --
`project()` in CMakeLists.txt, `display_name` from `rom_identity.txt` (or the
old `codegen_setup.c`), the zip prefix from `scripts/package_release.sh` --
using the template from the project's own snesrecomp submodule, so the
workflow matches the framework it pins. An installed workflow is compared by
step name: `--check` exits 1 when it is missing or stale, and only `--force`
overwrites one. It warns about anything the run will reach for and the
project lacks (`VERSION`, `framework_pins.txt`, `rom_identity.txt`, the
packager).

## What it produces

```text
<Title>SNESRecomp/
├── CMakeLists.txt          runner + generated C + host, netplay/multitap wired
├── VERSION                 release pin used for lobby version matching
├── framework_pins.txt      exact framework SHAs this project was cut against
├── recomp/                 bank*.cfg, symbols.toml — analysis input you own
├── rom_identity.txt        ROM digests + game_id — the one place a revision changes
├── mods/preloaded/         mod catalog (staged beside the exe; empty to start)
├── src/                    main.c (host shim), game_rtl.c, host_contract.c
│   └── gen/                generated C (gitignored — never committed)
├── tools/regen.sh          ROM → C, with digest verification
├── scripts/package_release.sh
├── .github/workflows/release.yml
└── snesrecomp/             framework submodule (owns lib/recomp-net, lib/retcomm-rbengine)
```

## Which framework the scaffold is cut from

The project pins `snesrecomp` as a submodule at the ref you name
(`--snesrecomp-ref`, default `main`), and every templated file is rendered
from THAT checkout's `tools/new_project/templates`, not from the copy of this
wizard that happens to be running. The two are the same only when you run the
wizard out of the checkout that becomes the submodule; Studio runs it from a
sibling checkout or its vendored copy, and a stale one there once rendered a
host that predated the framework it pinned: a project that built and did not
boot. The recomp-ui ref comes from the pinned framework's `RECOMP_UI_REF` the
same way. A wizard older than the framework it pins fails at the render with
the missing token named, rather than producing a project that is quietly
wrong.

## Title, boxart and metadata

The title is not asked: the probe names the project from the cartridge
header, then the filename (`Super Metroid`), and the folder and CMake name
follow (`SuperMetroidSNESRecomp`); `--name` overrides it. One question,
"Fetch boxart and metadata", covers the launcher's boxart (libretro
Named_Boxarts, `fetch_boxart.py`) and the README's publisher, developer and
year (libretro-database's per-system DATs keyed by the ROM's CRC32,
`fetch_metadata.py`). What the fetch cannot supply -- libretro carries no
marketing descriptions -- is asked afterwards, and only that. Scripted runs
stay offline unless `--fetch-boxart` says otherwise. When no boxart was
fetched, the wizard says where to put your own:
`launcher_assets/img/boxart.tga` (32-bit TGA, staged beside the executable on
the next build) and an optional `boxart.png` for the README.

## Pieces

| File | Role |
|------|------|
| `setup_project.sh` | The end-to-end driver |
| `probe_rom.py` | Cartridge-header identity: mapping, title, region, coprocessor, vectors, digests |
| `fill_tokens.py` | `@TOKEN@` substitution; unknown tokens are an error, not a blank |
| `templates/` | Everything written into the new repo |

## The workflow it writes

`.github/workflows/release.yml` builds four **setup packs** — Linux x86-64,
Windows x86-64, macOS arm64, macOS Intel — and can attach them to a GitHub
Release from a `v*` tag. It is manual-trigger only.

A setup pack is not the game. `src/gen/` is derived from a ROM that never
enters CI, so the pack holds the *setup host* (the executable built with
`-DSNESRECOMP_SETUP_HOST=ON`, without recompiled code), the recompiler, and
this source tree. On the player's machine the launcher's first-run wizard
takes their own ROM, generates, rebuilds, and relaunches into the real game.
The build tools (the retcomm `cmake-clang-v1` pack) are embedded in the zip by
default, or downloaded on first run.

See `snesrecomp/docs/ci/README.md` for the mechanics, and
`snesrecomp/docs/LOCAL_CODEGEN_SDK.md` for the launcher side.

`probe_rom.py` is usable on its own — `python3 probe_rom.py game.sfc` prints
what the scaffold would bake in, which is the fastest way to check whether a
title uses a coprocessor this runner supports before investing in a port.

## Options worth knowing

- `--players N` (1-8). Above two configures a Super Multitap: port 2 for 3-5
  seats (the layout commercial titles use), both ports for 6-8. Override with
  `--multitap`.
- `--rollback` builds `retcomm-rbengine` in and implies `--netplay`.
  Delay-sync stays the runtime default; `SNES_NET_MODE=rollback` opts in.
- `--recomp-net-ref` / `--rbengine-ref` follow a branch for the modules nested
  inside snesrecomp instead of the SHA the framework pins. CI always builds
  the committed gitlink SHAs.
- `--create-github` needs `gh`. Repo creation and push are separate steps by
  design (see above): the repo is created after the scaffold is committed and
  pushed once at the very end, so a re-run never produces a competing
  "initial" commit.
- `--no-submodules` scaffolds offline. The project will not build until you
  run `git submodule update --init --recursive`.
- `SNESRECOMP_ROOT=/path/to/checkout` makes the generate step use a working
  tree instead of the pinned submodule — for framework development.

## The ROM never enters the repository

The ROM is probed where it lies and is never copied in. `tools/regen.sh` takes
`--rom` (or `SNESRECOMP_ROM`) so it can stay on your own drive, and the
generated `.gitignore` blocks `*.sfc` / `*.smc` / `src/gen/` regardless.
`scripts/package_release.sh` refuses to build a zip that contains ROM data.

Which means the host has to *ask* for one, and the framework's desktop host
(`runner/src/desktop/host_main.c`, linked by `snesrecomp_target_desktop_host()`;
`src/main.c` is the shim that names the title) does — in this order, each candidate checked against the digests in
`rom_identity.txt`, the same ones the C was generated from. The build turns
that file into `snesrecomp_rom_identity.h`; `tools/regen.sh` and the release
workflow read it directly, so a revision bump is a one-line edit:

1. **The recomp-ui launcher** (`--recomp-ui`, on by default). A pre-boot GUI
   with a ROM picker and verification badge, plus display / audio / input
   settings, wired by one `recomp_target_launcher_ui(<target> CONSOLE snes)`
   call. It is skipped when a ROM is passed on the command line, and when
   `SDL_VIDEODRIVER=dummy` says nobody is there to answer it.
2. **`snesrecomp_launcher_resolve_rom_sha256()`** — the positional argument,
   then a copy beside the executable, then the `<exe_dir>/rom.cfg` cache, then
   a native file picker (zenity / kdialog / qarma / osascript).

Scaffolding with `--no-recomp-ui` keeps step 2 and compiles step 1 out; the
generated `CMakeLists.txt` carries the block to paste back in. What no longer
happens either way is the old behavior: printing a usage line and exiting 1
because the ROM was not already sitting in the working directory.

## Failure handling

The framework remotes are checked **before** anything is created, and a
failure part way through removes the directory the script made rather than
leaving a partial project the next run would refuse to overwrite.

The framework **URL and ref** both come from the checkout this script runs out
of, rather than being hard-coded — so a scaffold pins the framework that
checkout actually has. Hard-coding `main` silently produced projects that
could not generate or build whenever the work lived on a branch, which is the
normal state while a feature is in progress.

If that branch is not on the remote yet, the scaffolder says so, pins the
local commit anyway, and carries on: the project generates and builds here,
and pushing the branch later makes the pin resolvable for everyone else
without touching the project.

The pinned ref is then checked for the features the project asked for
(`generate`, recomp-net, retcomm-rbengine, `snesrecomp_enable_rollback`), so a
gap is reported up front instead of failing later with an argparse error that
names nothing.

## Tests

`tests/test_new_project.py` (in the framework suite, `python3
tests/run_tests.py`) runs the scaffolder against a synthetic, redistributable
image with `--no-submodules`, and checks the layout, that no `@TOKEN@`
survives, that the ROM digests live in `rom_identity.txt` and are NOT copied
into `regen.sh` / `main.c` / `CMakeLists.txt`, that multitap and rollback flags
reach CMake, and that no ROM is ever staged.
