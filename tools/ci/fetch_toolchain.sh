#!/usr/bin/env bash
# Download and unpack a retcomm-toolchains cmake-clang-v1 pack.
#
# The pack is the toolchain a setup zip's own rebuild uses on the player's
# machine (snesrecomp_codegen_host.c fetches the same asset). CI builds the
# setup host WITH it for that reason: if the pack cannot build this game, the
# player's rebuild cannot either, and CI is where that should be learned.
#
# Usage:
#   snesrecomp/tools/ci/fetch_toolchain.sh --artifact <linux-x64|windows-x64|macos-arm64|macos-x64>
#       [--tag vX.Y.Z] [--dl-dir DIR] [--out-dir DIR] [--repo OWNER/NAME]
#
# Prints TOOLCHAIN_DIR=<pack root with bin/>. Under Actions also appends
# TOOLCHAIN_DIR and RETCOMM_TOOLCHAIN_DIR to $GITHUB_ENV, and toolchain-dir to
# $GITHUB_OUTPUT.
#
# --tag pins a release; the default is the latest. A pinned tag is what a
# reproducible release wants; latest is what a nightly wants.
set -euo pipefail

ARTIFACT=""
TAG=""
DL_DIR=".cache/toolchain-dl"
OUT_DIR=".cache/toolchain-pack"
REPO="TechnicallyComputers/retcomm-toolchains"

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help) sed -n '2,19p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    --artifact) ARTIFACT="${2:?}"; shift 2 ;;
    --tag) TAG="${2:?}"; shift 2 ;;
    --dl-dir) DL_DIR="${2:?}"; shift 2 ;;
    --out-dir) OUT_DIR="${2:?}"; shift 2 ;;
    --repo) REPO="${2:?}"; shift 2 ;;
    *) echo "error: unknown arg: $1" >&2; exit 2 ;;
  esac
done

case "${ARTIFACT}" in
  linux-x64)              ASSET="cmake-clang-v1-linux-x64.zip" ;;
  windows-x64)            ASSET="cmake-clang-v1-windows-x64.zip" ;;
  macos-arm64|macos-x64)  ASSET="cmake-clang-v1-macos-universal.zip" ;;
  "") echo "error: --artifact is required" >&2; exit 2 ;;
  *) echo "error: unknown artifact '${ARTIFACT}'" >&2; exit 1 ;;
esac

# Reuse an unpacked pack (actions/cache restores OUT_DIR between runs).
find_pack_root() {
  local d
  for d in "${OUT_DIR}" "${OUT_DIR}"/*; do
    if [[ -d "${d}/bin" ]] && ls "${d}"/bin/cmake* >/dev/null 2>&1; then
      (cd "${d}" && pwd)
      return 0
    fi
  done
  return 1
}

emit() {
  local root="$1"
  if command -v cygpath >/dev/null 2>&1; then root="$(cygpath -m "${root}")"; fi
  echo "TOOLCHAIN_DIR=${root}"
  if [[ -n "${GITHUB_ENV:-}" ]]; then
    {
      echo "TOOLCHAIN_DIR=${root}"
      echo "RETCOMM_TOOLCHAIN_DIR=${root}"
    } >>"${GITHUB_ENV}"
  fi
  if [[ -n "${GITHUB_OUTPUT:-}" ]]; then
    echo "toolchain-dir=${root}" >>"${GITHUB_OUTPUT}"
  fi
}

if root="$(find_pack_root 2>/dev/null)"; then
  if [[ -f "${root}/retcomm-toolchain.json" ]]; then
    have="$(sed -n 's/.*"version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "${root}/retcomm-toolchain.json" | head -n1)"
    want="${TAG#v}"
    if [[ -z "${TAG}" || "${have}" == "${want}" ]]; then
      echo "toolchain pack already unpacked (${have:-unknown version}) at ${root}"
      emit "${root}"
      exit 0
    fi
    echo "cached pack is ${have}, want ${want}; refetching"
  fi
fi

rm -rf "${DL_DIR}" "${OUT_DIR}"
mkdir -p "${DL_DIR}" "${OUT_DIR}"

if [[ -n "${TAG}" ]]; then
  URL="https://github.com/${REPO}/releases/download/${TAG}/${ASSET}"
else
  URL="https://github.com/${REPO}/releases/latest/download/${ASSET}"
fi
echo "fetching ${URL}"
# HTTP/1.1: GitHub release assets intermittently REFUSED_STREAM under HTTP/2.
curl -fsSL --http1.1 --retry 5 --retry-all-errors --retry-delay 3 \
  -o "${DL_DIR}/${ASSET}" "${URL}"
SIZE="$(wc -c < "${DL_DIR}/${ASSET}" | tr -d '[:space:]')"
if [[ "${SIZE}" -lt 1000000 ]]; then
  echo "::error::${ASSET} is only ${SIZE} bytes — not a toolchain pack" >&2
  exit 1
fi

echo "unpacking ${ASSET}"
if command -v unzip >/dev/null 2>&1; then
  unzip -q "${DL_DIR}/${ASSET}" -d "${OUT_DIR}"
elif command -v tar >/dev/null 2>&1; then
  tar -xf "${DL_DIR}/${ASSET}" -C "${OUT_DIR}"
else
  python3 -c 'import sys,zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' \
    "${DL_DIR}/${ASSET}" "${OUT_DIR}"
fi
rm -f "${DL_DIR}/${ASSET}"

# Zip extraction does not preserve the executable bit everywhere (Python's
# zipfile never does). The pack is useless without it.
find "${OUT_DIR}" -type d \( -name bin -o -path '*/python/bin' \) -exec chmod -R a+x {} + 2>/dev/null || true

root="$(find_pack_root)" || {
  echo "::error::unpacked ${ASSET} but found no bin/cmake under ${OUT_DIR}" >&2
  find "${OUT_DIR}" -maxdepth 2 >&2 || true
  exit 1
}
echo "toolchain pack ready: $(sed -n 's/.*"version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "${root}/retcomm-toolchain.json" 2>/dev/null | head -n1)"
emit "${root}"
