#!/usr/bin/env bash
# Print — and optionally verify — the framework revisions a build used.
#
# A port is its game tree PLUS the framework revisions pinned beside it, so a
# build log that names only the game commit cannot answer "were these two
# binaries the same build?". That question is asked after every desync, and
# answering it forensically is expensive.
#
# Usage (from the game repo root):
#   snesrecomp/tools/ci/record_pins.sh [--root DIR] [--check FILE]
#
# --check compares the live gitlinks against a committed pin file (one
# `name=<40-hex sha>` per line, e.g. framework_pins.txt) and exits non-zero on
# a mismatch. Without it this only reports.
set -euo pipefail

ROOT="${PWD}"
CHECK=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --root) ROOT="${2:?}"; shift 2 ;;
    --check) CHECK="${2:?}"; shift 2 ;;
    -h|--help)
      sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *)
      echo "error: unknown arg: $1" >&2
      exit 2
      ;;
  esac
done

ROOT="$(cd "${ROOT}" && pwd)"

# name -> path, in the order a reader wants them.
PIN_NAMES="snesrecomp recomp-ui recomp-net retcomm-rbengine"
pin_path() {
  case "$1" in
    snesrecomp)       echo "snesrecomp" ;;
    recomp-ui)        echo "recomp-ui" ;;
    recomp-net)       echo "snesrecomp/lib/recomp-net" ;;
    retcomm-rbengine) echo "snesrecomp/lib/retcomm-rbengine" ;;
    *)                echo "" ;;
  esac
}

live_sha() {
  local path="$1"
  if [[ -e "${ROOT}/${path}/.git" ]]; then
    git -C "${ROOT}/${path}" rev-parse HEAD
  else
    echo ""
  fi
}

status=0
echo "Framework revisions in this build:"
for name in ${PIN_NAMES}; do
  path="$(pin_path "${name}")"
  sha="$(live_sha "${path}")"
  if [[ -z "${sha}" ]]; then
    if [[ -d "${ROOT}/${path}" ]]; then
      echo "  ${name}=<present at ${path}, not a git checkout>"
    else
      echo "  ${name}=<missing: ${path}>"
    fi
    continue
  fi
  echo "  ${name}=${sha}  (${sha:0:12})"
done

if [[ -z "${CHECK}" ]]; then
  exit 0
fi

if [[ ! -f "${ROOT}/${CHECK}" && ! -f "${CHECK}" ]]; then
  echo "::error::--check ${CHECK}: no such file" >&2
  exit 1
fi
PINFILE="${CHECK}"
[[ -f "${ROOT}/${CHECK}" ]] && PINFILE="${ROOT}/${CHECK}"

echo
echo "Comparing against ${CHECK}:"
while IFS= read -r line; do
  line="${line%%#*}"
  line="$(printf '%s' "${line}" | tr -d '[:space:]')"
  [[ -z "${line}" ]] && continue
  [[ "${line}" != *=* ]] && continue
  name="${line%%=*}"
  want="${line#*=}"
  path="$(pin_path "${name}")"
  if [[ -z "${path}" ]]; then
    echo "  ${name}: not a known framework component — ignored"
    continue
  fi
  got="$(live_sha "${path}")"
  if [[ -z "${got}" ]]; then
    echo "::error::${name}: pinned ${want:0:12} but ${path} is not checked out"
    status=1
  elif [[ "${got}" != "${want}" ]]; then
    echo "::error::${name}: pinned ${want:0:12} but checked out ${got:0:12}"
    status=1
  else
    echo "  ${name}: ok (${got:0:12})"
  fi
done < "${PINFILE}"

exit "${status}"
