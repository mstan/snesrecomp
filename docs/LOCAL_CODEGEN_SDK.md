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
(same contract as the host). Metal Warriors is one consumer; other titles copy
the thin config pattern from its `src/codegen_setup.c`.
