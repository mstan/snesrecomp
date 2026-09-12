#!/usr/bin/env bash
# Package a SETUP HOST release: a zip a player unpacks, runs, and turns into
# the playable game with their own ROM.
#
# What goes in:
#   <exe>              the setup host (built with -DSNESRECOMP_SETUP_HOST=ON)
#   assets/            launcher fonts and images, from beside the built exe
#   <runtime dirs>     e.g. mods/, translations/ -- whatever the game loads
#                      exe-relative, from beside the built exe (--runtime-dir)
#   <source tree>      every git-tracked file, submodules included: the
#                      launcher's Generate & rebuild recompiles THIS tree
#   toolchain/         optional (--embed-toolchain): the cmake-clang-v1 pack,
#                      so nothing is downloaded on first run
#   README.txt, VERSION
#
# What never goes in: the ROM, src/gen/*.c, recomp/funcs.h, or anything else
# derived from ROM bytes. That is enforced below, not assumed.
#
# Usage (from the game repo root):
#   snesrecomp/tools/ci/stage_setup_host.sh \
#       --build-dir build-ci --artifact linux-x64 \
#       --exe-name MyGameSNESRecomp --zip-prefix mygame --display-name "My Game" \
#       [--runtime-dir NAME]... [--rom-hint "My Game (USA).sfc"] \
#       [--rom-sha256 HEX] [--rom-crc32 HEX] \
#       [--embed-toolchain] [--toolchain-dir DIR] [--root DIR]
#
# Env: RELEASE_VERSION overrides the VERSION file. RETCOMM_TOOLCHAIN_DIR /
# TOOLCHAIN_DIR name the pack for --embed-toolchain when --toolchain-dir is
# not given.
set -euo pipefail

# A silent exit is the worst outcome a packaging step can have: the first
# Windows CI run died here with exit 1 and not one line of output. Name the
# command and line instead of leaving the reader to bisect a shell script.
trap 'echo "::error::$(basename "${BASH_SOURCE[0]}") failed at line ${LINENO}: ${BASH_COMMAND}" >&2' ERR


SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="${PWD}"
BUILD_DIR=""
ARTIFACT=""
EXE_NAME=""
ZIP_PREFIX=""
DISPLAY_NAME=""
ROM_HINT="your legally owned ROM"
ROM_SHA256=""
ROM_CRC32=""
EMBED_TOOLCHAIN=0
TOOLCHAIN_DIR_ARG=""
RUNTIME_DIRS=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help) sed -n '2,29p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    --build-dir) BUILD_DIR="${2:?}"; shift 2 ;;
    --artifact) ARTIFACT="${2:?}"; shift 2 ;;
    --exe-name) EXE_NAME="${2:?}"; shift 2 ;;
    --zip-prefix) ZIP_PREFIX="${2:?}"; shift 2 ;;
    --display-name) DISPLAY_NAME="${2:?}"; shift 2 ;;
    --rom-hint) ROM_HINT="${2:?}"; shift 2 ;;
    --rom-sha256) ROM_SHA256="${2:?}"; shift 2 ;;
    --rom-crc32) ROM_CRC32="${2:?}"; shift 2 ;;
    --runtime-dir) RUNTIME_DIRS+=("${2:?}"); shift 2 ;;
    --embed-toolchain) EMBED_TOOLCHAIN=1; shift ;;
    --no-embed-toolchain) EMBED_TOOLCHAIN=0; shift ;;
    --toolchain-dir) TOOLCHAIN_DIR_ARG="${2:?}"; shift 2 ;;
    --root) ROOT="${2:?}"; shift 2 ;;
    *) echo "error: unknown arg: $1" >&2; exit 2 ;;
  esac
done

for v in BUILD_DIR ARTIFACT EXE_NAME ZIP_PREFIX DISPLAY_NAME; do
  if [[ -z "${!v}" ]]; then
    echo "error: --$(echo "${v}" | tr '[:upper:]_' '[:lower:]-') is required" >&2
    exit 2
  fi
done

ROOT="$(cd "${ROOT}" && pwd)"
cd "${ROOT}"
VERSION="${RELEASE_VERSION:-$(tr -d '[:space:]' < VERSION)}"
if [[ -z "${VERSION}" ]]; then
  echo "error: VERSION is empty" >&2
  exit 1
fi

NAME="${ZIP_PREFIX}-${VERSION}-${ARTIFACT}"
DIST="${ROOT}/dist"
STAGE="${DIST}/${NAME}"
ZIP="${DIST}/${NAME}.zip"

# ── the setup host ──────────────────────────────────────────────────────────
# .exe candidates FIRST. Under MSYS/Git Bash, [[ -f foo ]] is true when only
# foo.exe exists (the runtime maps the name), so probing the bare name first
# staged an extensionless path that no native tool -- Python, 7z, the loader
# -- could open. On Linux/macOS no .exe exists and the bare name wins.
EXE=""
for cand in "${BUILD_DIR}/${EXE_NAME}.exe" "${BUILD_DIR}/Release/${EXE_NAME}.exe" \
            "${BUILD_DIR}/${EXE_NAME}"; do
  if [[ -f "${cand}" ]]; then EXE="${cand}"; break; fi
done
if [[ -z "${EXE}" ]]; then
  echo "error: ${EXE_NAME} not found under ${BUILD_DIR} — build the setup host first:" >&2
  echo "  cmake -S . -B ${BUILD_DIR} -DCMAKE_BUILD_TYPE=Release -DSNESRECOMP_SETUP_HOST=ON" >&2
  exit 1
fi
EXE_DIR="$(cd "$(dirname "${EXE}")" && pwd)"

# A setup host is defined by what it lacks. If the build tree it came from
# had generated C, this binary is a real game build and MUST NOT ship as a
# "setup" zip whose README promises otherwise.
if grep -q 'SNESRECOMP_SETUP_HOST' "${BUILD_DIR}/CMakeCache.txt" 2>/dev/null; then
  if ! grep -q '^SNESRECOMP_SETUP_HOST:BOOL=ON' "${BUILD_DIR}/CMakeCache.txt"; then
    echo "error: ${BUILD_DIR} was not configured with -DSNESRECOMP_SETUP_HOST=ON" >&2
    exit 1
  fi
fi

rm -rf "${STAGE}"
mkdir -p "${STAGE}"
cp -a "${EXE}" "${STAGE}/"
STAGE_EXE="${STAGE}/$(basename "${EXE}")"

# ── launcher assets + exe-relative runtime dirs ─────────────────────────────
if [[ -d "${EXE_DIR}/assets/fonts" ]]; then
  mkdir -p "${STAGE}/assets"
  cp -a "${EXE_DIR}/assets/." "${STAGE}/assets/"
else
  echo "error: ${EXE_DIR}/assets/fonts missing — the launcher would open with no text." >&2
  echo "  recomp_ui.cmake stages it POST_BUILD; rebuild ${EXE_NAME}." >&2
  exit 1
fi
for d in "${RUNTIME_DIRS[@]+"${RUNTIME_DIRS[@]}"}"; do
  [[ -n "${d}" ]] || continue
  if [[ ! -d "${EXE_DIR}/${d}" ]]; then
    echo "error: --runtime-dir ${d}: ${EXE_DIR}/${d} missing — build the target that stages it" >&2
    exit 1
  fi
  mkdir -p "${STAGE}/${d}"
  cp -a "${EXE_DIR}/${d}/." "${STAGE}/${d}/"
  # Per-machine mod state never ships.
  rm -f "${STAGE}/${d}/state.toml" "${STAGE}/${d}/state.toml.tmp"
done

# ── the source tree the player's rebuild compiles ───────────────────────────
# Tracked files only, submodules included, no .git directories. tar rather
# than cp --parents: macOS cp has no --parents, and bsdtar and GNU tar both
# take --null -T -.
if ! git -C "${ROOT}" rev-parse --git-dir >/dev/null 2>&1; then
  echo "error: ${ROOT} is not a git checkout; cannot enumerate the source tree" >&2
  exit 1
fi
# GNU tar treats -C as positional: it must come before -T. The existence
# filter keeps a local tree with an uncommitted deletion from aborting the
# whole copy (CI checkouts never have one; a developer's tree can).
git -C "${ROOT}" ls-files -z --recurse-submodules \
  | grep -zvE '(^|/)\.github/' \
  | { while IFS= read -r -d '' f; do [[ -e "${ROOT}/${f}" ]] && printf '%s\0' "${f}"; done; } \
  | tar -C "${ROOT}" --null -T - -cf - \
  | tar -xf - -C "${STAGE}"
# Where things came from, for whoever debugs a player's rebuild.
{
  echo "game=$(git -C "${ROOT}" rev-parse HEAD)"
  git -C "${ROOT}" submodule status --recursive | sed 's/^[ +-]//; s/ (.*//' \
    | awk '{print $2"="$1}'
} > "${STAGE}/SOURCE_REVISIONS"

# The launcher finds its project root by these two files. Prove the staged
# tree has them, or the shipped zip can generate but never rebuild.
for need in snesrecomp/snesrecomp_cli.py recomp/bank00.cfg CMakeLists.txt VERSION; do
  if [[ ! -f "${STAGE}/${need}" ]]; then
    echo "error: staged tree lacks ${need}; the setup host could not find its own project" >&2
    exit 1
  fi
done

# ── optional embedded toolchain ─────────────────────────────────────────────
if [[ "${EMBED_TOOLCHAIN}" -eq 1 ]]; then
  TC="${TOOLCHAIN_DIR_ARG:-${RETCOMM_TOOLCHAIN_DIR:-${TOOLCHAIN_DIR:-}}}"
  if [[ -z "${TC}" || ! -d "${TC}/bin" ]]; then
    echo "error: --embed-toolchain needs a pack: pass --toolchain-dir or set RETCOMM_TOOLCHAIN_DIR" >&2
    exit 1
  fi
  echo "embedding toolchain from ${TC}"
  mkdir -p "${STAGE}/toolchain"
  cp -a "${TC}/." "${STAGE}/toolchain/"
  # Never ship the packager's ccache contents or a stale build cache.
  rm -rf "${STAGE}/toolchain/.ccache" "${STAGE}/toolchain/cache" 2>/dev/null || true
fi

# ── shared libraries beside the executable ──────────────────────────────────
bash "${SCRIPT_DIR}/bundle_runtime_libs.sh" --exe "${STAGE_EXE}" --build-dir "${EXE_DIR}"

# ── README + VERSION ────────────────────────────────────────────────────────
printf '%s\n' "${VERSION}" > "${STAGE}/VERSION"
{
  echo "${DISPLAY_NAME} ${VERSION} — setup pack (${ARTIFACT})"
  echo
  echo "This pack does NOT contain the game. It contains a setup program, the"
  echo "recompiler, and the source code, and it builds the game on your machine"
  echo "from your own copy of the ROM. No ROM data is included or downloaded."
  echo
  echo "First run:"
  echo "  1. Start $(basename "${EXE}")."
  if [[ "${EMBED_TOOLCHAIN}" -eq 1 ]]; then
    echo "  2. The build tools are included (toolchain/); nothing to download."
  else
    echo "  2. Let it download the portable build tools (cmake-clang-v1, one time"
    echo "     per machine, shared with other recompiled games), or point it at a"
    echo "     cmake-clang-v1 zip you downloaded yourself."
  fi
  echo "  3. Pick your ROM: ${ROM_HINT}"
  if [[ -n "${ROM_CRC32}${ROM_SHA256}" ]]; then
    [[ -n "${ROM_CRC32}"  ]] && echo "       CRC32   ${ROM_CRC32}"
    [[ -n "${ROM_SHA256}" ]] && echo "       SHA-256 ${ROM_SHA256}"
  fi
  echo "  4. Generate & rebuild. The playable game is written to build/ and"
  echo "     started for you; run it from there afterwards."
  echo
  echo "Requirements: about 2 GB of disk during the build."
  case "${ARTIFACT}" in
    macos-*) echo "macOS: the Xcode Command Line Tools must be installed (xcode-select --install)." ;;
    linux-*) echo "Linux: glibc 2.35 or newer (Ubuntu 22.04+, SteamOS 3.x)." ;;
  esac
  echo
  echo "SOURCE_REVISIONS lists the exact commits this pack was cut from."
} > "${STAGE}/README.txt"

# ── timestamps ──────────────────────────────────────────────────────────────
# Zip entries carry local time with no zone. CI runs in UTC; a player who
# unpacks in a western zone gets every source file stamped hours in the
# FUTURE, the build's own outputs stay "older" than their inputs, and ninja
# regenerates build.ninja until it gives up: "manifest 'build.ninja' still
# dirty after 100 tries, perhaps system time is not set" -- with nothing
# compiled. Stamp every staged file with the game commit's time, which is
# in the past in every zone, and reproducible besides.
# GNU touch takes -d @epoch; BSD touch (macOS) does not. Both take an ISO
# YYYY-MM-DDThh:mm:SS, so convert the epoch with whichever date is here.
#
# Two days BEFORE the commit, not the commit time itself. The zip stores
# whatever wall-clock string the runner writes, and the runner is UTC; a
# player in UTC-4 reading "16:59" gets a file four hours in the future, which
# is how the first fix failed. No zone is more than 26 hours from another,
# so 48 hours back is in the past everywhere, and still reproducible.
EPOCH="$(git -C "${ROOT}" log -1 --format=%ct 2>/dev/null || date +%s)"
EPOCH=$((EPOCH - 172800))
STAMP="$(date -u -d "@${EPOCH}" +%Y-%m-%dT%H:%M:%S 2>/dev/null \
      || date -u -r "${EPOCH}" +%Y-%m-%dT%H:%M:%S)"
find "${STAGE}" -exec touch -h -d "${STAMP}" {} + 2>/dev/null \
  || find "${STAGE}" -exec touch -d "${STAMP}" {} +

# ── the licensing rule, enforced ────────────────────────────────────────────
if find "${STAGE}" \( -iname '*.sfc' -o -iname '*.smc' -o -iname '*.srm' -o -iname '*.fig' \) | grep -q .; then
  echo "error: refusing to package — ROM files in ${STAGE}:" >&2
  find "${STAGE}" \( -iname '*.sfc' -o -iname '*.smc' -o -iname '*.srm' -o -iname '*.fig' \) >&2
  exit 1
fi
if find "${STAGE}/src/gen" -name '*.c' 2>/dev/null | grep -q . || [[ -f "${STAGE}/recomp/funcs.h" ]]; then
  echo "error: refusing to package — generated C in the stage (src/gen/*.c or recomp/funcs.h)." >&2
  echo "  A setup pack ships no recompiler OUTPUT. Clean the tree and rebuild." >&2
  exit 1
fi

# ── zip ─────────────────────────────────────────────────────────────────────
rm -f "${ZIP}"
(
  cd "${DIST}"
  if command -v zip >/dev/null 2>&1; then
    zip -qr -y "${NAME}.zip" "${NAME}"
  elif command -v 7z >/dev/null 2>&1; then
    # Errors stay visible (-bsp0 only silences the progress bar).
    7z a -tzip -bsp0 "${NAME}.zip" "${NAME}"
  else
    # Python's zipfile does not keep the executable bit; only reached on a
    # host with neither zip nor 7z, which the workflow does not use.
    python3 - "${NAME}" <<'PY'
import os, stat, sys, zipfile
name = sys.argv[1]
with zipfile.ZipFile(name + ".zip", "w", zipfile.ZIP_DEFLATED) as zf:
    for root, _, files in os.walk(name):
        for f in files:
            p = os.path.join(root, f)
            zi = zipfile.ZipInfo.from_file(p, p)
            zi.external_attr = (os.stat(p).st_mode & 0xFFFF) << 16
            with open(p, "rb") as fp:
                zf.writestr(zi, fp.read())
PY
  fi
)
rm -rf "${STAGE}"
echo "Packaged: ${ZIP}"
ls -l "${ZIP}"
