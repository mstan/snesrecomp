#!/usr/bin/env bash
# Make an SDL3 development package available to `find_package(SDL3 CONFIG)`.
#
# runner.cmake defaults to SDL3 and resolves it with find_package(SDL3 CONFIG
# REQUIRED) — there is no FetchContent fallback, so a runner without SDL3
# development files fails at CONFIGURE time with a message that reads like a
# project bug rather than a missing dependency. The four release runners do
# not agree on how to supply it:
#
#   macOS      brew has sdl3
#   Windows    MSYS2 MINGW64 has mingw-w64-x86_64-SDL3
#   Linux      Ubuntu 24.04 (ubuntu-latest) has NO libsdl3-dev — SDL3 postdates
#              noble. Newer images may gain it, so this tries apt first and
#              builds from the pinned release tarball only when it has to.
#
# One script rather than three per-OS workflow blocks, because the divergence
# is the whole difficulty and a workflow that inlines it grows a fourth
# variant the next time a runner image moves.
#
# Usage:
#   snesrecomp/tools/ci/provision_sdl3.sh [--prefix DIR] [--cache-dir DIR]
#                                         [--version X.Y.Z] [--print-only]
#
# Prints SDL3_DIR=<dir containing SDL3Config.cmake>. Under Actions it also
# appends SDL3_DIR and CMAKE_PREFIX_PATH to $GITHUB_ENV, so the configure step
# needs no per-OS -D flags.
#
# Env overrides:
#   SNESRECOMP_SDL3_VERSION / SNESRECOMP_SDL3_SHA256 / SNESRECOMP_SDL3_URL
set -euo pipefail

VERSION="${SNESRECOMP_SDL3_VERSION:-3.4.10}"
SHA256="${SNESRECOMP_SDL3_SHA256:-12b34280415ec8418c864408b93d008a20a6530687ee613d60bfbd20411f2785}"
URL="${SNESRECOMP_SDL3_URL:-}"
PREFIX=""
CACHE_DIR=""
PRINT_ONLY=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --prefix) PREFIX="${2:?}"; shift 2 ;;
    --cache-dir) CACHE_DIR="${2:?}"; shift 2 ;;
    --version) VERSION="${2:?}"; shift 2 ;;
    --print-only) PRINT_ONLY=1; shift ;;
    -h|--help) sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "error: unknown arg: $1" >&2; exit 2 ;;
  esac
done

WS="${GITHUB_WORKSPACE:-${PWD}}"
[[ -n "${PREFIX}" ]]    || PREFIX="${WS}/.cache/sdl3-prefix"
[[ -n "${CACHE_DIR}" ]] || CACHE_DIR="${WS}/.cache"
[[ -n "${URL}" ]]       || URL="https://github.com/libsdl-org/SDL/releases/download/release-${VERSION}/SDL3-${VERSION}.tar.gz"

abs() {
  if command -v cygpath >/dev/null 2>&1; then cygpath -m "$1"; else (cd "$1" && pwd); fi
}

# find_package(SDL3 CONFIG) wants the directory holding SDL3Config.cmake.
# Search a prefix the way CMake would, so a hit here means a hit there.
config_dir_under() {
  local root="$1" d
  [[ -d "${root}" ]] || return 1
  for d in \
      "${root}/lib/cmake/SDL3" \
      "${root}/lib64/cmake/SDL3" \
      "${root}/lib/x86_64-linux-gnu/cmake/SDL3" \
      "${root}/cmake" \
      "${root}/lib/cmake/SDL3-${VERSION}"; do
    if [[ -f "${d}/SDL3Config.cmake" ]]; then
      abs "${d}"
      return 0
    fi
  done
  d="$(find "${root}" -name SDL3Config.cmake -type f 2>/dev/null | head -n1 || true)"
  if [[ -n "${d}" ]]; then
    abs "$(dirname "${d}")"
    return 0
  fi
  return 1
}

emit() {
  local dir="$1" origin="$2"
  local root
  root="$(cd "${dir}/../../.." 2>/dev/null && pwd || echo "")"
  echo "SDL3 ${origin}"
  echo "SDL3_DIR=${dir}"
  if [[ -n "${GITHUB_ENV:-}" && "${PRINT_ONLY}" -eq 0 ]]; then
    {
      echo "SDL3_DIR=${dir}"
      if [[ -n "${root}" ]]; then
        echo "CMAKE_PREFIX_PATH=${root}${CMAKE_PREFIX_PATH:+:${CMAKE_PREFIX_PATH}}"
        # A syntax-only host check compiles with pkg-config rather than CMake,
        # and would otherwise miss an SDL3 that is not on the default path.
        for pc in "${root}/lib/pkgconfig" "${root}/lib64/pkgconfig" \
                  "${root}/lib/x86_64-linux-gnu/pkgconfig"; do
          if [[ -f "${pc}/sdl3.pc" ]]; then
            echo "PKG_CONFIG_PATH=${pc}${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"
            break
          fi
        done
      fi
    } >>"${GITHUB_ENV}"
  fi
}

UNAME="$(uname -s)"

# 1. Already provisioned by an earlier run (or a warm actions/cache).
if dir="$(config_dir_under "${PREFIX}" 2>/dev/null)"; then
  emit "${dir}" "already built at ${PREFIX}"
  exit 0
fi

# 2. Whatever the platform's package manager offers.
case "${UNAME}" in
  Darwin)
    if ! brew list sdl3 >/dev/null 2>&1; then
      echo "Installing SDL3 via Homebrew…"
      brew install sdl3
    fi
    if dir="$(config_dir_under "$(brew --prefix sdl3)")"; then
      emit "${dir}" "from Homebrew"
      exit 0
    fi
    echo "::error::brew installed sdl3 but SDL3Config.cmake was not found" >&2
    exit 1
    ;;
  MINGW*|MSYS*|CYGWIN*)
    root="${MINGW_PREFIX:-/mingw64}"
    if ! dir="$(config_dir_under "${root}" 2>/dev/null)"; then
      echo "Installing mingw-w64-x86_64-SDL3…"
      pacman -S --noconfirm --needed mingw-w64-x86_64-SDL3
      dir="$(config_dir_under "${root}")" || {
        echo "::error::MSYS2 SDL3 package installed but SDL3Config.cmake was not found" >&2
        exit 1
      }
    fi
    emit "${dir}" "from MSYS2 (${root})"
    exit 0
    ;;
esac

# Linux. An SDL3 already visible to CMake wins — a self-hosted runner or a
# newer image may ship one, and building a second copy would only mask that.
for root in /usr /usr/local; do
  if dir="$(config_dir_under "${root}" 2>/dev/null)"; then
    emit "${dir}" "already installed (${root})"
    exit 0
  fi
done

# apt next; a distro package is cheaper and better tested than ours.
if command -v apt-get >/dev/null 2>&1; then
  if apt-cache show libsdl3-dev >/dev/null 2>&1; then
    echo "Installing libsdl3-dev…"
    sudo apt-get install -y --no-install-recommends libsdl3-dev
    for root in /usr /usr/local; do
      if dir="$(config_dir_under "${root}" 2>/dev/null)"; then
        emit "${dir}" "from apt (${root})"
        exit 0
      fi
    done
    echo "warning: libsdl3-dev installed but no SDL3Config.cmake; building from source" >&2
  else
    echo "libsdl3-dev not in the apt index — building SDL3 ${VERSION} from source."
  fi
fi

# 3. Build the pinned release tarball. A build is slow, so it is the last
#    resort and its output goes in --prefix, which the workflow caches.
mkdir -p "${CACHE_DIR}"
TARBALL="${CACHE_DIR}/SDL3-${VERSION}.tar.gz"
SRC="${CACHE_DIR}/sdl3-src"

if [[ ! -f "${SRC}/CMakeLists.txt" ]]; then
  if [[ ! -f "${TARBALL}" ]]; then
    echo "Downloading ${URL}"
    # HTTP/1.1: GitHub release assets intermittently REFUSED_STREAM under HTTP/2.
    curl -fsSL --http1.1 --retry 5 --retry-all-errors --retry-delay 2 \
      -o "${TARBALL}.part" "${URL}"
    mv -f "${TARBALL}.part" "${TARBALL}"
  fi
  if command -v sha256sum >/dev/null 2>&1; then
    echo "${SHA256}  ${TARBALL}" | sha256sum -c -
  elif command -v shasum >/dev/null 2>&1; then
    echo "${SHA256}  ${TARBALL}" | shasum -a 256 -c -
  else
    echo "::error::no sha256 tool — refusing to build an unverified SDL3 tarball" >&2
    exit 1
  fi
  rm -rf "${SRC}.extracting" "${SRC}"
  mkdir -p "${SRC}.extracting"
  tar -xzf "${TARBALL}" -C "${SRC}.extracting"
  inner="$(find "${SRC}.extracting" -mindepth 1 -maxdepth 1 -type d | head -n1)"
  if [[ -z "${inner}" || ! -f "${inner}/CMakeLists.txt" ]]; then
    echo "::error::unexpected SDL3 tarball layout" >&2
    ls -la "${SRC}.extracting" >&2 || true
    exit 1
  fi
  mv "${inner}" "${SRC}"
  rm -rf "${SRC}.extracting"
fi

JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
GEN=()
command -v ninja >/dev/null 2>&1 && GEN=(-G Ninja)
echo "Building SDL3 ${VERSION} → ${PREFIX}"
cmake -S "${SRC}" -B "${CACHE_DIR}/sdl3-build" "${GEN[@]}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
  -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_EXAMPLES=OFF
cmake --build "${CACHE_DIR}/sdl3-build" -j"${JOBS}"
cmake --install "${CACHE_DIR}/sdl3-build"

dir="$(config_dir_under "${PREFIX}")" || {
  echo "::error::SDL3 install under ${PREFIX} has no SDL3Config.cmake" >&2
  exit 1
}
emit "${dir}" "built from source ${VERSION}"
