# Release CI for a snesrecomp port

Four platform builds — Linux x86-64, Windows x86-64, macOS arm64, macOS Intel
— from one workflow, packaged as player zips and attachable to a GitHub
Release.

The template is `tools/new_project/templates/release.yml.in`. There is one
copy of it: `setup_project.sh --ci` fills its tokens into a new project's
`.github/workflows/release.yml`. Do not fork the template per title — a leaf
that re-implements a rule cannot inherit fixes to it. Title-specific
differences belong in the filled workflow (extra `-D` flags, extra build
targets) and in `scripts/package_release.sh`.

## The constraint everything follows from

`src/gen/*.c` is recompiler output derived from ROM bytes. It is not
committed, the ROM is never stored in CI, and CI cannot regenerate it. So a
workflow either gets the generated C from somewhere, or it is not building the
game — and the one thing it must never do is blur those two into the same
green tick.

The template resolves that with a repository variable:

| `CI_SRC_GEN_ASSET` | what runs | what a green tick means |
| --- | --- | --- |
| set | `build` (4 platforms) + `release` | four real game binaries were produced |
| unset | `ROM-free host check (no game build)` | the framework tests pass and the host sources still compile |

Exactly one of the two runs. Publishing a release with the variable unset is
refused in `preflight` rather than producing an empty release.

## Setting up the real build

1. Regenerate locally from your own verified ROM: `bash tools/regen.sh`.
2. Publish the recompiler output to a private assets repository:

   ```sh
   snesrecomp/tools/ci/publish_src_gen.sh \
       --repo OWNER/psxrecomp-ci-assets \
       --path <game-slug> \
       --include src/gen \
       --include recomp/funcs.h      # if your build needs it and gitignores it
   ```

   `--include` is repeatable and repo-relative; paths are stored
   repo-root-relative so the fetch side unzips at the root. The script refuses
   to publish anything ROM-shaped. Add a thin `tools/publish_ci_src_gen.sh`
   wrapper in the game repo so nobody has to remember the arguments.

3. In the game repo's settings add:
   - secret `CI_ASSETS_TOKEN` — fine-grained PAT, **Contents: Read** on the
     assets repo.
   - variable `CI_SRC_GEN_ASSET` — e.g. `<game-slug>/src-gen.zip`.
   - variable `CI_SRC_GEN_REPO` — only if the assets repo is not
     `TechnicallyComputers/psxrecomp-ci-assets`.

Fork pull requests cannot read repository secrets. That is why the workflow
triggers on `workflow_dispatch` and `v*` tags rather than on pull requests.

## Cutting a release

`VERSION` is the lobby version pin compiled into the binary: two peers on
different strings are refused a seat rather than allowed to desync later. So
the tag and `VERSION` must agree, and `preflight` fails when they do not.

```sh
printf '0.2.0\n' > VERSION && git commit -am 'Release 0.2.0'
git tag v0.2.0 && git push origin main v0.2.0
```

The `release` job requires all four zips; a run that produced three fails
rather than publishing a partial release.

## Shared scripts (`tools/ci/`)

One implementation per problem, for every port:

| script | what it is for |
| --- | --- |
| `record_pins.sh` | print the framework revisions a build used; `--check framework_pins.txt` turns drift into an error |
| `provision_sdl3.sh` | make SDL3 findable by `find_package(SDL3 CONFIG)` on all four runners |
| `fetch_src_gen.sh` | restore the generated C from the assets repo (`--from-file` for local testing) |
| `publish_src_gen.sh` | the producer side of the above |
| `bundle_runtime_libs.sh` | copy the shared libraries a staged executable needs next to it |

### Why `provision_sdl3.sh` exists

`runner.cmake` defaults to SDL3 and resolves it with `find_package(SDL3
CONFIG REQUIRED)`; there is no FetchContent fallback, so a runner without SDL3
development files fails at *configure* time with a message that reads like a
project bug. The four runners do not agree on how to supply it: Homebrew and
MSYS2 both package SDL3, but **Ubuntu 24.04 — which is `ubuntu-latest` — does
not**, because SDL3 postdates noble. The script tries the system, then apt,
then builds the pinned release tarball into `.cache/sdl3-prefix`, which the
workflow caches. Newer runner images that gain `libsdl3-dev` are picked up
automatically without a workflow edit.

### Why `bundle_runtime_libs.sh` exists

A zip that runs on the machine that built it and nowhere else is the default
outcome on all three platforms, for three unrelated reasons: MinGW links
libgcc/libstdc++/libwinpthread and SDL3 as DLLs beside the compiler; Homebrew
dylibs carry absolute `/opt/homebrew` paths; a source-built SDL3 on Linux
lives in a CI cache directory. The script walks the binary's imports
transitively on Windows, runs `dylibbundler` and then *verifies* no absolute
paths remain on macOS, and on Linux copies only what a player's distro may not
have — refusing to copy anything unless the executable has an `$ORIGIN`
RUNPATH, because a bundled library the loader cannot find is worse than an
honest missing dependency.

The Linux workflow therefore configures with `-DCMAKE_BUILD_RPATH='$ORIGIN'`.
A fully static SDL3 (what the retcomm toolchain pack produces) leaves nothing
to bundle, and that is reported as a finished step, not a skipped one.

## Runner images

| runner | artifact |
| --- | --- |
| `ubuntu-24.04` | `linux-x64` |
| `windows-2022` (MSYS2 MINGW64) | `windows-x64` |
| `macos-15` | `macos-arm64` |
| `macos-15-intel` | `macos-x64` |

macOS builds pass `-DCMAKE_OSX_DEPLOYMENT_TARGET=11.0`. They are two separate
single-architecture packs rather than one universal binary, because the games
link Homebrew dependencies that are themselves per-architecture.
