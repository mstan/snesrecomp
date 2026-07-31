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

## Metal Warriors driver

Game repos should call this CLI instead of invoking `v2_emit.py` directly.
Metal Warriors wraps it from `tools/regen.sh`.
