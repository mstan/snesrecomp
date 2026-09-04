#!/usr/bin/env bash
# Publish this port's generated C to a private CI assets repository.
#
# The producer side of fetch_src_gen.sh. Run it locally after a regen against
# your own ROM; CI then builds the real game without the ROM ever leaving your
# machine.
#
# What goes in: recompiler OUTPUT only (src/gen/*.c and whatever else the
# build needs but does not commit, e.g. recomp/funcs.h). What never goes in:
# the ROM, or anything that is a copy of ROM bytes rather than code derived
# from them. The check below is mechanical, because "I would never" is not a
# guard.
#
# Usage (from the game repo root):
#   snesrecomp/tools/ci/publish_src_gen.sh \
#       --repo OWNER/NAME --path <game-dir> [--include PATH]... [--dry-run]
#
# --include is repeatable and repo-relative; the default set is `src/gen`.
# Paths are stored repo-root-relative, so the fetch side unzips at the root.
set -euo pipefail

REPO=""
DEST_DIR=""
DRY_RUN=0
ROOT="${PWD}"
INCLUDES=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --repo) REPO="${2:?}"; shift 2 ;;
    --path) DEST_DIR="${2:?}"; shift 2 ;;
    --include) INCLUDES+=("${2:?}"); shift 2 ;;
    --root) ROOT="${2:?}"; shift 2 ;;
    --dry-run) DRY_RUN=1; shift ;;
    -h|--help) sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "error: unknown arg: $1" >&2; exit 2 ;;
  esac
done

if [[ -z "${REPO}" || -z "${DEST_DIR}" ]]; then
  echo "error: --repo OWNER/NAME and --path <game-dir> are required" >&2
  exit 2
fi
if [[ "${#INCLUDES[@]}" -eq 0 ]]; then
  INCLUDES=(src/gen)
fi

ROOT="$(cd "${ROOT}" && pwd)"
cd "${ROOT}"

STAGE="$(mktemp -d)"
trap 'rm -rf "${STAGE}"' EXIT
mkdir -p "${STAGE}/payload"

for item in "${INCLUDES[@]}"; do
  if [[ ! -e "${item}" ]]; then
    echo "error: --include ${item}: not found (run tools/regen.sh first)" >&2
    exit 1
  fi
  mkdir -p "${STAGE}/payload/$(dirname "${item}")"
  cp -a "${item}" "${STAGE}/payload/$(dirname "${item}")/"
done

# No ROM bytes leave this tree. Enforced here rather than trusted, and by
# extension not left to the assets repo to notice later.
if find "${STAGE}/payload" \( -name '*.sfc' -o -name '*.smc' -o -name '*.sfc.*' \
      -o -name '*.smc.*' -o -name '*.srm' -o -name '*.bin' \) | grep -q .; then
  echo "error: refusing to publish — ROM-shaped files in the payload:" >&2
  find "${STAGE}/payload" \( -name '*.sfc' -o -name '*.smc' -o -name '*.srm' \
      -o -name '*.bin' \) >&2
  exit 1
fi

ZIP="${STAGE}/src-gen.zip"
( cd "${STAGE}/payload" && zip -9 -q -r "${ZIP}" . )
SIZE="$(wc -c < "${ZIP}" | tr -d '[:space:]')"
echo "Archive: ${SIZE} bytes"
( cd "${STAGE}/payload" && find . -type f | sed 's|^\./|  |' | sort )

OUT="${ROOT}/dist/ci-assets/${DEST_DIR}"
mkdir -p "${OUT}"
cp -f "${ZIP}" "${OUT}/src-gen.zip"
echo "Wrote ${OUT}/src-gen.zip"

if [[ "${DRY_RUN}" -eq 1 ]]; then
  echo "(--dry-run) not pushing to ${REPO}"
  exit 0
fi

if ! command -v gh >/dev/null 2>&1; then
  echo "error: gh CLI required to push; the archive is ready at ${OUT}/src-gen.zip" >&2
  exit 1
fi

CLONE="$(mktemp -d)"
trap 'rm -rf "${STAGE}" "${CLONE}"' EXIT
export GIT_LFS_SKIP_SMUDGE=1
TOKEN="$(gh auth token)"
git clone --depth 1 "https://x-access-token:${TOKEN}@github.com/${REPO}.git" "${CLONE}"
mkdir -p "${CLONE}/${DEST_DIR}"
cp -f "${OUT}/src-gen.zip" "${CLONE}/${DEST_DIR}/src-gen.zip"

GAME_SHA="$(git -C "${ROOT}" rev-parse HEAD 2>/dev/null || echo unknown)"
cat > "${CLONE}/${DEST_DIR}/README.md" <<EOF
# ${DEST_DIR} CI assets

\`src-gen.zip\` is recompiler OUTPUT (repo-root-relative paths), restored by
\`snesrecomp/tools/ci/fetch_src_gen.sh\` so GitHub Actions can link the game.

No ROM is stored here.

Published from ${DEST_DIR} at commit ${GAME_SHA}.
Refresh after a regen with \`snesrecomp/tools/ci/publish_src_gen.sh\`.
EOF

cd "${CLONE}"
git add "${DEST_DIR}/src-gen.zip" "${DEST_DIR}/README.md"
if git diff --cached --quiet; then
  echo "No change — ${REPO} already has this archive."
  exit 0
fi
git -c user.email="ci@localhost" -c user.name="CI assets" \
  commit -q -m "Update ${DEST_DIR} src-gen.zip for CI (no ROM)."
git push -q origin HEAD
echo "Published ${REPO}:${DEST_DIR}/src-gen.zip"
