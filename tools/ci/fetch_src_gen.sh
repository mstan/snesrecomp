#!/usr/bin/env bash
# Restore a port's generated C from a private CI assets repository.
#
# src/gen/*.c is recompiler output derived from a copyrighted ROM, so it is
# never committed and the ROM is never stored in CI. A CI job that wants to
# build the actual game therefore has to get the generated C from somewhere,
# and the only honest "somewhere" is an archive a human produced locally from
# their own ROM (snesrecomp/tools/ci/publish_src_gen.sh) and pushed to a
# private repo the workflow can read with a token.
#
# There is deliberately no fallback. If the archive cannot be fetched this
# exits non-zero: a job that quietly built something OTHER than the game while
# still reporting green is worse than a red job, because the green is what
# gets trusted at release time.
#
# Usage (from the game repo root):
#   snesrecomp/tools/ci/fetch_src_gen.sh \
#       --repo OWNER/NAME --asset <game>/src-gen.zip [--root DIR] [--expect GLOB]
#
# --from-file <zip> skips the download and restores an archive already on
# disk. That is how the round trip gets tested without a token, and how a
# self-hosted runner with the archive staged locally uses this.
#
# Env:
#   CI_ASSETS_TOKEN / GH_TOKEN / GITHUB_TOKEN — read token for --repo.
set -euo pipefail

REPO=""
ASSET=""
ROOT="${PWD}"
EXPECT="src/gen/*.c"
FROM_FILE=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --repo) REPO="${2:?}"; shift 2 ;;
    --asset) ASSET="${2:?}"; shift 2 ;;
    --root) ROOT="${2:?}"; shift 2 ;;
    --expect) EXPECT="${2:?}"; shift 2 ;;
    --from-file) FROM_FILE="${2:?}"; shift 2 ;;
    -h|--help) sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "error: unknown arg: $1" >&2; exit 2 ;;
  esac
done

if [[ -z "${FROM_FILE}" && ( -z "${REPO}" || -z "${ASSET}" ) ]]; then
  echo "::error::fetch_src_gen.sh needs --repo OWNER/NAME and --asset PATH (or --from-file)" >&2
  exit 2
fi

ROOT="$(cd "${ROOT}" && pwd)"
TMP="${RUNNER_TEMP:-${TMPDIR:-/tmp}}/snesrecomp-src-gen.zip"

if [[ -n "${FROM_FILE}" ]]; then
  if [[ ! -f "${FROM_FILE}" ]]; then
    echo "::error::--from-file ${FROM_FILE}: no such file" >&2
    exit 1
  fi
  TMP="${FROM_FILE}"
  echo "Restoring from ${FROM_FILE} (no download)"
else
  TOKEN="${CI_ASSETS_TOKEN:-${GH_TOKEN:-${GITHUB_TOKEN:-}}}"
  if [[ -z "${TOKEN}" ]]; then
    {
      echo "::error::no token for ${REPO}."
      echo "Set the repository secret CI_ASSETS_TOKEN to a fine-grained PAT with"
      echo "Contents: Read on ${REPO}. Pull requests from forks cannot read"
      echo "repository secrets — run this workflow from the main repo"
      echo "(push / workflow_dispatch)."
    } >&2
    exit 1
  fi

  rm -f "${TMP}"

  # Contents API rather than a checkout: it fetches one file with one token, and
  # does not drag LFS blobs or the rest of a shared assets repo onto the runner.
  API="https://api.github.com/repos/${REPO}/contents/${ASSET}"
  # Not curl -f: the HTTP status is the diagnosis and -f throws it away.
  CODE="$(curl -sS -L -o "${TMP}" -w '%{http_code}' \
    -H "Authorization: Bearer ${TOKEN}" \
    -H "Accept: application/vnd.github.raw" \
    -H "X-GitHub-Api-Version: 2022-11-28" \
    "${API}" || echo "000")"

  if [[ "${CODE}" != "200" ]]; then
    echo "::error::GET ${REPO}/${ASSET} returned HTTP ${CODE}" >&2
    case "${CODE}" in
      401|403) echo "  token rejected — check CI_ASSETS_TOKEN has Contents: Read on ${REPO}" >&2 ;;
      404)     echo "  not found — check the path, and that a publish has run for this game" >&2 ;;
      000)     echo "  curl could not reach api.github.com" >&2 ;;
    esac
    rm -f "${TMP}"
    exit 1
  fi

  SIZE="$(wc -c < "${TMP}" | tr -d '[:space:]')"
  if [[ "${SIZE}" -lt 512 ]]; then
    echo "::error::${ASSET} is only ${SIZE} bytes — that is an error page, not an archive" >&2
    head -c 400 "${TMP}" >&2 || true
    rm -f "${TMP}"
    exit 1
  fi
fi

PY="$(command -v python3 || command -v python)"
if [[ -z "${PY}" ]]; then
  echo "::error::python3 is required to extract the archive" >&2
  exit 1
fi

# Entries are repo-root-relative paths written by publish_src_gen.sh. Extract
# through Python rather than unzip so an entry that escapes the repo root is a
# hard error instead of a file written somewhere nobody looks.
SRC_GEN_ZIP="${TMP}" SRC_GEN_ROOT="${ROOT}" "${PY}" - <<'PY'
import os
import sys
import zipfile
from pathlib import Path

zip_path = Path(os.environ["SRC_GEN_ZIP"])
root = Path(os.environ["SRC_GEN_ROOT"]).resolve()

written = []
with zipfile.ZipFile(zip_path) as zf:
    for info in zf.infolist():
        name = info.filename
        if name.endswith("/"):
            continue
        dest = (root / name).resolve()
        if not str(dest).startswith(str(root) + os.sep):
            sys.exit(f"archive entry escapes the repo root: {name!r}")
        dest.parent.mkdir(parents=True, exist_ok=True)
        with zf.open(info) as fp, open(dest, "wb") as out:
            out.write(fp.read())
        written.append(name)

if not written:
    sys.exit("archive is empty")
for name in sorted(written):
    print(f"  restored {name}")
print(f"Restored {len(written)} file(s) from {zip_path.name}")
PY

[[ -n "${FROM_FILE}" ]] || rm -f "${TMP}"

# The archive can be well formed and still be the wrong archive. Prove the
# thing the build actually needs is now on disk.
shopt -s nullglob
matches=(${ROOT}/${EXPECT})
shopt -u nullglob
if [[ "${#matches[@]}" -eq 0 ]]; then
  echo "::error::archive extracted but ${EXPECT} is still empty — wrong asset for this game?" >&2
  exit 1
fi
echo "Generated C ready: ${#matches[@]} file(s) matching ${EXPECT}"
