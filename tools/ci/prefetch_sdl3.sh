#!/usr/bin/env bash
# Prefetch the pinned SDL3 source tree for runner.cmake's FetchContent fallback.
#
# Only needed where no SDL3 package exists and the toolchain pack ships none
# (macOS: the pack is cmake + ninja, Xcode supplies clang). CMake's own
# downloader speaks HTTP/2 to GitHub release assets and intermittently dies
# with REFUSED_STREAM; curl --http1.1 does not. Point CMake at the result with
#   -DSNESRECOMP_SDL3_SOURCE_DIR=<out-dir>
# (this script appends that variable to $GITHUB_ENV under Actions).
#
# Usage:
#   snesrecomp/tools/ci/prefetch_sdl3.sh [--out-dir DIR] [--version X.Y.Z]
# The default version and digest match runner.cmake and retcomm-toolchains.
set -euo pipefail

VERSION="3.4.10"
SHA256="12b34280415ec8418c864408b93d008a20a6530687ee613d60bfbd20411f2785"
OUT_DIR=".cache/sdl3-src"

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help) sed -n '2,14p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    --out-dir) OUT_DIR="${2:?}"; shift 2 ;;
    --version) VERSION="${2:?}"; shift 2 ;;
    --sha256) SHA256="${2:?}"; shift 2 ;;
    *) echo "error: unknown arg: $1" >&2; exit 2 ;;
  esac
done

emit() {
  local abs
  abs="$(cd "${OUT_DIR}" && pwd)"
  if command -v cygpath >/dev/null 2>&1; then abs="$(cygpath -m "${abs}")"; fi
  echo "SDL3 source: ${abs}"
  [[ -n "${GITHUB_ENV:-}" ]] && echo "SNESRECOMP_SDL3_SOURCE_DIR=${abs}" >>"${GITHUB_ENV}"
  return 0
}

if [[ -f "${OUT_DIR}/CMakeLists.txt" ]]; then
  echo "SDL3 ${VERSION} source already present"
  emit
  exit 0
fi

CACHE="$(dirname "${OUT_DIR}")"
mkdir -p "${CACHE}"
TAR="${CACHE}/SDL3-${VERSION}.tar.gz"
URL="https://github.com/libsdl-org/SDL/releases/download/release-${VERSION}/SDL3-${VERSION}.tar.gz"
echo "fetching ${URL}"
curl -fsSL --http1.1 --retry 5 --retry-all-errors --retry-delay 2 -o "${TAR}.part" "${URL}"
mv -f "${TAR}.part" "${TAR}"
if command -v sha256sum >/dev/null 2>&1; then
  echo "${SHA256}  ${TAR}" | sha256sum -c -
else
  echo "${SHA256}  ${TAR}" | shasum -a 256 -c -
fi
rm -rf "${OUT_DIR}.extracting"
mkdir -p "${OUT_DIR}.extracting"
tar -xzf "${TAR}" -C "${OUT_DIR}.extracting"
inner="$(find "${OUT_DIR}.extracting" -mindepth 1 -maxdepth 1 -type d | head -n1)"
[[ -n "${inner}" && -f "${inner}/CMakeLists.txt" ]] || {
  echo "::error::unexpected SDL3 tarball layout" >&2; exit 1; }
rm -rf "${OUT_DIR}"
mv "${inner}" "${OUT_DIR}"
rm -rf "${OUT_DIR}.extracting" "${TAR}"
emit
