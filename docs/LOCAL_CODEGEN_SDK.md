# Local codegen SDK

Headless contract for regenerating an existing SNESRecomp game project from a
user-supplied ROM. Intended for `recomp-ui` setup flows and RetComM launcher
automation. This does **not** redistribute ROM data.

## Commands

```bash
python snesrecomp_cli.py verify-rom --rom GAME.sfc \
  --expected-crc32 f2ab92d4 \
  --expected-sha256 <64-hex> \
  [--json-progress]

python snesrecomp_cli.py generate \
  --rom GAME.sfc \
  --cfg-dir recomp \
  --out-dir src/gen \
  --funcs-h recomp/funcs.h \
  --project-root /path/to/GameRecomp \
  --cfg-roots \
  [--expected-crc32 ...] \
  [--expected-sha256 ...] \
  [--analysis-backend auto|python|native] \
  [--no-host-root-scan] \
  [--source-root PATH]... \
  [--json-progress]
```

`build` remains the greenfield scaffolder (new empty project). `generate`
targets an existing title that already has `bank*.cfg` seeds.

## Exit codes

| Code | Meaning |
|------|---------|
| 0 | success |
| 1 | runtime / generation failure |
| 2 | usage / argument error |
| 3 | ROM verification failure |

## JSONL progress (`--json-progress`)

Stdout is reserved for one JSON object per line. Useful events:

| `event` | Notes |
|---------|--------|
| `phase` | `phase`, optional `pct` / `message` (`verify`, `emit`, `sync_funcs_h`, `done`) |
| `rom` | digests after verification |
| `log` | mirrored tool chatter |
| `result` | final payload (`ok`, `rom`, `out_dir`, …) |
| `error` | `message`, `code`, optional `details` |

Human-readable text goes to stderr when JSON progress is enabled.

## Portable recomp-ui host (`host/`)

Any game that embeds recomp-ui can reuse the full setup flow (pick ROM →
generate → cmake rebuild → relaunch) by compiling:

- `host/snesrecomp_codegen_host.c`
- `host/snesrecomp_codegen_host.h`

Put `recomp-ui/src` and `snesrecomp/host` on the include path, fill a
`SnesrecompCodegenHostConfig`, then:

```c
snesrecomp_codegen_host_apply(&gi, &my_cfg);

/* after recomp_launcher_run_window: */
if (lr == RECOMP_LAUNCHER_RESULT_RELAUNCH)
    snesrecomp_codegen_host_relaunch_or_exit(rom_path);
```

### Config fields (minimum)

| Field | Example |
|-------|---------|
| `display_name` | `"My Game"` |
| `cmake_target` | `"MyGameSNESRecomp"` |
| `exe_basename` | `"MyGameSNESRecomp"` |
| `expected_crc32` / `expected_sha256` | optional digests |
| `cfg_roots` | `1` for typical ports |

Defaults cover `recomp/`, `src/gen/`, `snesrecomp/snesrecomp_cli.py`, and
`build/`. Override with `SNESRECOMP_PROJECT_ROOT`, `SNESRECOMP_BUILD_DIR`,
`SNESRECOMP_FORCE_SETUP` (or per-game env names in the config).

### Platform rebuild behavior

| OS | Behavior |
|----|----------|
| Linux / macOS | In-process `cmake --build`, then `exec` the new binary |
| Windows | Writes `build/recomp_deferred_rebuild.cmd`, exits; helper waits for the game PID, builds, starts the new exe (avoids a locked `.exe`) |

Folder layout stays `build/` on every OS so RetComM and other tools can treat
projects uniformly.

### Game regen scripts

CLI wrappers such as `tools/regen.sh` should call `snesrecomp_cli.py generate`
(same contract as the host), reading their digests from `rom_identity.txt`.

Titles no longer copy a config pattern. Generate & rebuild is wired by
`snesrecomp_codegen_host_autowire()`, which takes no configuration: the CMake
target and the binary to relaunch are the running executable's own name, and
every path it needs is a scaffolder convention with a default in
`snesrecomp_codegen_host_apply()`. A port that copies configuration is a port
that cannot inherit a fix to it.

## Setup packs (release zips without generated C)

A release zip is a **setup pack**: the host built with
`-DSNESRECOMP_SETUP_HOST=ON` (no `src/gen`, empty dispatch tables from
`runner/src/setup_host_dispatch.c`, `SnesInit()` refuses to boot), plus the
recompiler and source tree. The wizard above is the only path forward in that
binary. `docs/ci/README.md` covers how CI builds and packages one.

For that to work from a zip the host does three things a source checkout
never needed:

| step | behaviour |
|------|-----------|
| project root | also searched upward from the **executable's directory**, not just the cwd — a double-clicked zip has an unrelated cwd |
| toolchain | `setup_needs_toolchain` + `toolchain_is_ready` / `ensure_toolchain_with_progress` / `toolchain_update_available`: finds a `cmake-clang-v1` pack (`RETCOMM_TOOLCHAIN_DIR`, `toolchain/` beside the exe, the RetComM cache) or downloads it from `TechnicallyComputers/retcomm-toolchains` into that cache; falls back to cmake/cc/python on `PATH` |
| configure | when `build/CMakeCache.txt` is absent, `cmake -S -B -G Ninja -DCMAKE_BUILD_TYPE=Release` runs before `--build`, with the pack's `env.sh` sourced (POSIX) or `env.bat` called (the Windows deferred helper) |

`setup_wizard_supported` is set by `apply()`; without it recomp-ui keeps the
wizard dark even with every callback wired, which is what an SNES host built
before this change did.
