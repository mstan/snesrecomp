#!/usr/bin/env sh
# Write the release workflow into an existing SNES project (Linux / macOS /
# WSL / Git Bash). The logic is tools/generate_ci.py; this only finds Python.
#
#   sh snesrecomp/tools/generate_ci.sh                 # cwd is the project
#   sh snesrecomp/tools/generate_ci.sh ~/src/MyGame    # or name the root
#   sh snesrecomp/tools/generate_ci.sh --check         # stale? (exit 1)
#   sh snesrecomp/tools/generate_ci.sh --force         # overwrite
#
# Every flag passes through: --display-name, --project-name, --zip-prefix,
# --dry-run. `--help` prints the full list.
set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PYTHON=${PYTHON:-$(command -v python3 || command -v python || true)}
[ -n "$PYTHON" ] || { echo "generate_ci: python3 not found on PATH" >&2; exit 1; }
exec "$PYTHON" "$SCRIPT_DIR/generate_ci.py" "$@"
