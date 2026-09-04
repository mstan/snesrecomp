# Release CI for a snesrecomp port: setup packs

Four platform builds — Linux x86-64, Windows x86-64, macOS arm64, macOS Intel
— from one manual-trigger workflow, packaged as **setup packs** and
attachable to a GitHub Release.

The template is `tools/new_project/templates/release.yml.in`; there is one
copy, and `setup_project.sh --ci` fills its tokens into a new project's
`.github/workflows/release.yml`. Do not fork it per title — a leaf that
re-implements a rule cannot inherit fixes to it. Title-specific differences
belong in the filled workflow (extra `-D` flags, extra build targets) and in
the thin `scripts/package_release.sh` wrapper (runtime dirs, ROM digests).

## What a setup pack is

`src/gen/*.c` is recompiler output derived from ROM bytes. It is not
committed, the ROM is never stored in CI, and CI cannot regenerate it. So CI
does not build the game. It builds the **setup host**:

```
cmake -S . -B build-ci -DCMAKE_BUILD_TYPE=Release -DSNESRECOMP_SETUP_HOST=ON
```

`snesrecomp_target_generated_code(<target> src/gen)` (runner.cmake) sees an
empty `src/gen/` plus that option and links `runner/src/setup_host_dispatch.c`
— empty dispatch tables — instead of failing configure. `SnesInit()` in that
build refuses to boot a guest at all and says why. Nothing in the binary can
run guest code with an invented result; its only reachable path is the
launcher's first-run wizard.

The zip then carries:

| in the zip | from |
|---|---|
| the setup host | `build-ci/` |
| `assets/` (launcher fonts, boxart) | beside the built exe (`recomp_ui.cmake` stages them) |
| runtime dirs (`mods/`, `translations/`, …) | beside the built exe, per `--runtime-dir`; the game stages them with `snesrecomp_target_stage_dir()` so the player's rebuild gets them too |
| the whole source tree, submodules included | `git ls-files --recurse-submodules` |
| `toolchain/` (optional) | the fetched `cmake-clang-v1` pack |
| `README.txt`, `VERSION`, `SOURCE_REVISIONS` | generated |

Never: the ROM, `src/gen/*.c`, `recomp/funcs.h`. `stage_setup_host.sh`
checks the stage for all three and refuses to zip. It also refuses a build
directory not configured with `SNESRECOMP_SETUP_HOST=ON`, and a stage that
lacks `snesrecomp/snesrecomp_cli.py` or `recomp/bank00.cfg` — the two files
the launcher uses to recognise its project root; without them the shipped
host could generate but never rebuild.

## What happens on the player's machine

`host/snesrecomp_codegen_host.c`, wired by `snesrecomp_codegen_host_autowire()`
in the game's `main.c`, drives the recomp-ui first-run wizard:

1. **Toolchain.** Looks for a `cmake-clang-v1` pack: `RETCOMM_TOOLCHAIN_DIR`,
   then `toolchain/` beside the executable (embedded), then the RetComM cache
   (`%LOCALAPPDATA%\retcomm\toolchains\cmake-clang-v1`, or
   `$XDG_DATA_HOME`/`~/.local/share/retcomm/toolchains/cmake-clang-v1`), then
   cmake + a C compiler + python on `PATH`. If none, the wizard offers to
   download the pack from `TechnicallyComputers/retcomm-toolchains` (or take a
   zip the player fetched) into that cache — shared with RetComM and psxrecomp
   hosts, so one download per machine.
2. **ROM.** The player picks their copy; digests are checked.
3. **Generate.** `snesrecomp_cli.py generate` with the pack's Python (or
   `PATH`'s), streaming JSONL progress.
4. **Configure + build.** First time: `cmake -S <root> -B build -G Ninja
   -DCMAKE_BUILD_TYPE=Release` with the pack's `env.sh` sourced (its own
   statement of PATH / CC / sysroot / SDL3_DIR). Then `cmake --build`.
   Windows defers this to a helper `.cmd` after exit, because the running
   `.exe` cannot be rebuilt in place.
5. **Relaunch** into `build/<exe>`.

`SNESRECOMP_SDL3_FETCH` (runner.cmake, default ON) builds SDL3 from the
pinned release when no package is found — the macOS pack ships no SDL3, and
neither does a player's machine.

## Cutting a release

Open the Actions page, pick **Release**, press **Run workflow**. The defaults
are the release: the next version is chosen for you, all four packs are
built, the commit you ran from is tagged, and a public GitHub Release is
published with the zips and a `SHA256SUMS`. No push, pull request or schedule
ever runs it — a run is a decision a person makes.

| input | default | meaning |
| --- | --- | --- |
| `publish_release` | on | tag + publish; off = dry run, artifacts only |
| `version` | empty | release exactly this `X.Y.Z` |
| `bump` | `patch` | with `version` empty: bump this part of the latest `v*` tag |
| `embed_toolchain` | on | embed cmake-clang-v1 in each zip (Linux ~800 MB); off = lean zip that downloads on first run |
| `toolchain_tag` | empty | pin a retcomm-toolchains release; empty = latest |
| `verify_pins` | off | fail on drift from `framework_pins.txt` |

How the version is chosen, in order: `version` if given; the tag itself if
the run was started from a `v*` tag (re-release); `VERSION` if no tag exists
yet (the first release); otherwise the latest tag bumped — unless `VERSION`
in the tree is ahead of that, in which case the maintainer's deliberate bump
wins. That version is the lobby pin compiled into the binary and written into
the pack's `VERSION`, so the tag, the setup host and the player's own rebuild
agree. The tag is created only after all four builds succeed, so a failed
build never claims a number.

## Shared scripts (`tools/ci/`)

| script | what it is for |
| --- | --- |
| `record_pins.sh` | print the framework revisions a build used; `--check framework_pins.txt` turns drift into an error |
| `fetch_toolchain.sh` | download + unpack the `cmake-clang-v1` pack for the runner (cached by `actions/cache`) |
| `prefetch_sdl3.sh` | curl the pinned SDL3 tarball for the FetchContent fallback (macOS) |
| `stage_setup_host.sh` | build the setup pack; every rule above lives here |
| `bundle_runtime_libs.sh` | copy shared libraries beside a staged executable (usually nothing: SDL3 is static and MinGW links `-static`) |

## Runner images

| runner | artifact | compiler |
| --- | --- | --- |
| `ubuntu-24.04` | `linux-x64` | pack clang + jammy sysroot → runs on glibc ≥ 2.35 |
| `windows-2022` | `windows-x64` | pack llvm-mingw, `-static` runtime |
| `macos-15` | `macos-arm64` | Xcode clang; pack supplies cmake/ninja/python |
| `macos-15-intel` | `macos-x64` | same |

All four jobs run in `bash` — the Windows pack ships an `env.sh`, so MSYS2 is
not involved. Two separate macOS packs rather than one universal binary,
because the player's rebuild is single-architecture anyway.
