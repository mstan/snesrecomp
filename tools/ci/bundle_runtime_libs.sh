#!/usr/bin/env bash
# Copy the shared libraries a staged executable needs next to it.
#
# A release zip that runs on the machine that built it and nowhere else is the
# default outcome on all three platforms, for three different reasons: MinGW
# links libgcc/libstdc++/libwinpthread and SDL3 as DLLs beside the compiler;
# Homebrew dylibs carry absolute /opt/homebrew paths; a source-built SDL3 on
# Linux lives in a CI cache directory that does not exist on a player's disk.
#
# This is one implementation of that for every SNES port, because getting it
# wrong produces a zip that looks complete and fails at launch — the failure
# mode a packaging step exists to prevent.
#
# Usage:
#   snesrecomp/tools/ci/bundle_runtime_libs.sh --exe <staged exe> \
#       [--stage DIR] [--build-dir DIR] [--strict]
#
# --stage defaults to the staged exe's directory. --strict turns "a library
# the binary imports could not be found" into an error on every platform (it
# already is on Windows).
set -euo pipefail

# A silent exit is the worst outcome a packaging step can have: the first
# Windows CI run died here with exit 1 and not one line of output. Name the
# command and line instead of leaving the reader to bisect a shell script.
trap 'echo "::error::$(basename "${BASH_SOURCE[0]}") failed at line ${LINENO}: ${BASH_COMMAND}" >&2' ERR


EXE=""
STAGE=""
BUILD_DIR=""
STRICT=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --exe) EXE="${2:?}"; shift 2 ;;
    --stage) STAGE="${2:?}"; shift 2 ;;
    --build-dir) BUILD_DIR="${2:?}"; shift 2 ;;
    --strict) STRICT=1; shift ;;
    -h|--help) sed -n '2,23p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "error: unknown arg: $1" >&2; exit 2 ;;
  esac
done

if [[ -z "${EXE}" || ! -f "${EXE}" ]]; then
  echo "error: --exe <staged executable> is required and must exist" >&2
  exit 2
fi
[[ -n "${STAGE}" ]] || STAGE="$(cd "$(dirname "${EXE}")" && pwd)"
[[ -n "${BUILD_DIR}" ]] || BUILD_DIR="${STAGE}"

case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) PLATFORM=windows ;;
  Darwin)               PLATFORM=macos ;;
  *)                    PLATFORM=linux ;;
esac
# A Windows binary can also be staged from a cross build; trust the name too.
[[ "${EXE}" == *.exe ]] && PLATFORM=windows

copied=0
copy_lib() {
  local src="$1"
  local base
  base="$(basename "${src}")"
  if [[ -f "${STAGE}/${base}" ]]; then
    return 0
  fi
  cp -f "${src}" "${STAGE}/${base}"
  echo "  bundled ${base}"
  copied=$((copied + 1))
}

case "${PLATFORM}" in
windows)
  OBJDUMP=""
  for cand in x86_64-w64-mingw32-objdump objdump llvm-objdump; do
    if command -v "${cand}" >/dev/null 2>&1; then OBJDUMP="${cand}"; break; fi
  done
  # Git Bash on a Windows runner has no binutils; the llvm-mingw toolchain
  # pack does. Look there before giving up.
  if [[ -z "${OBJDUMP}" && -n "${RETCOMM_TOOLCHAIN_DIR:-}" ]]; then
    for cand in llvm-objdump.exe llvm-objdump x86_64-w64-mingw32-objdump.exe; do
      if [[ -x "${RETCOMM_TOOLCHAIN_DIR}/bin/${cand}" ]]; then OBJDUMP="${RETCOMM_TOOLCHAIN_DIR}/bin/${cand}"; break; fi
    done
  fi
  if [[ -z "${OBJDUMP}" ]]; then
    echo "::error::no objdump on PATH — cannot determine the DLLs $(basename "${EXE}") imports" >&2
    exit 1
  fi

  # Where MinGW runtime DLLs live: MSYS2's /mingw64/bin, or the llvm-mingw
  # toolchain pack (bin/ for the host arch, plus the target sysroot's bin/).
  # With SNESRECOMP_STATIC_RUNTIME (the default) none are imported and this
  # walk finds nothing to copy -- which is the intended outcome, not a skip.
  RUNTIME_BINS=("${MINGW_PREFIX:-/mingw64}/bin")
  if [[ -n "${RETCOMM_TOOLCHAIN_DIR:-}" ]]; then
    RUNTIME_BINS+=("${RETCOMM_TOOLCHAIN_DIR}/bin" "${RETCOMM_TOOLCHAIN_DIR}/x86_64-w64-mingw32/bin")
  fi
  # Windows' own DLLs ship with the OS; copying them is at best noise and at
  # worst a version conflict with the loader's.
  SYSTEM_RE='^(KERNEL32|KERNELBASE|USER32|GDI32|GDIPLUS|ADVAPI32|SHELL32|SHCORE|OLE32|OLEAUT32|WS2_32|WINMM|IMM32|SETUPAPI|VERSION|OPENGL32|GLU32|COMCTL32|COMDLG32|RPCRT4|SHLWAPI|CRYPT32|BCRYPT|NCRYPT|IPHLPAPI|NSI|DNSAPI|MSVCRT|UCRTBASE|VCRUNTIME[0-9]*|MSVCP[0-9]*|DBGHELP|DWMAPI|UXTHEME|POWRPROF|CFGMGR32|HID|WINTRUST|MSIMG32|AVRT|MF[A-Z]*|AUDIOSES|DINPUT8|XINPUT[0-9_]*|API-MS-.*|EXT-MS-.*)\.DLL$'

  imports_of() {
    "${OBJDUMP}" -p "$1" 2>/dev/null | awk '/DLL Name:/{print $3}' | sort -u
  }

  # Walk transitively: SDL3.dll itself pulls in the MinGW runtime DLLs, and a
  # zip carrying SDL3.dll without them is the same broken zip one level down.
  pending="$(imports_of "${EXE}")"
  seen=""
  while [[ -n "${pending}" ]]; do
    next=""
    for dll in ${pending}; do
      upper="$(printf '%s' "${dll}" | tr '[:lower:]' '[:upper:]')"
      case " ${seen} " in *" ${upper} "*) continue ;; esac
      seen="${seen} ${upper}"
      if printf '%s' "${upper}" | grep -qE "${SYSTEM_RE}"; then
        continue
      fi
      src=""
      for dir in "$(dirname "${EXE}")" "${BUILD_DIR}" "${RUNTIME_BINS[@]}"; do
        if [[ -f "${dir}/${dll}" ]]; then src="${dir}/${dll}"; break; fi
      done
      if [[ -z "${src}" ]]; then
        echo "::error::required DLL not found: ${dll}" >&2
        echo "  looked in $(dirname "${EXE}"), ${BUILD_DIR}, ${RUNTIME_BINS[*]}" >&2
        exit 1
      fi
      copy_lib "${src}"
      next="${next} $(imports_of "${src}")"
    done
    pending="${next}"
  done
  ;;

macos)
  if ! command -v dylibbundler >/dev/null 2>&1; then
    if command -v brew >/dev/null 2>&1; then
      echo "Installing dylibbundler…"
      brew install dylibbundler
    fi
  fi
  if ! command -v dylibbundler >/dev/null 2>&1; then
    echo "::error::dylibbundler not available — the zip would depend on this runner's Homebrew tree" >&2
    exit 1
  fi
  mkdir -p "${STAGE}/libs"
  # -od overwrite, -b bundle, -x the binary, -p the install_name prefix the
  # loader will use at runtime.
  dylibbundler -od -b -x "${EXE}" -d "${STAGE}/libs" -p "@executable_path/libs/"
  copied="$(find "${STAGE}/libs" -type f -name '*.dylib' | wc -l | tr -d '[:space:]')"
  # Prove it: any remaining absolute dependency outside the system prefixes is
  # a library the player's machine will not have.
  leftover="$(otool -L "${EXE}" | tail -n +2 | awk '{print $1}' \
    | grep -vE '^(/usr/lib/|/System/|@executable_path/|@rpath/|@loader_path/)' || true)"
  if [[ -n "${leftover}" ]]; then
    echo "::error::executable still links absolute paths that will not exist on a player's Mac:" >&2
    printf '  %s\n' ${leftover} >&2
    [[ "${STRICT}" -eq 1 ]] && exit 1
    exit 1
  fi
  ;;

linux)
  if ! command -v ldd >/dev/null 2>&1; then
    echo "warning: ldd missing; skipping Linux library bundling" >&2
    exit 0
  fi

  # Bundle anything resolved from outside the system library directories (a
  # CI-built SDL3 lives in a cache dir), plus SDL3 wherever it came from: the
  # player's distro is not guaranteed to package it at all. The C library, the
  # loader, and the GCC runtime are deliberately NOT bundled — they have to
  # match the player's kernel and their GL driver, and overriding them beside
  # the executable is a well-known way to break both.
  CANDIDATES=""
  for lib in $(ldd "${EXE}" 2>/dev/null | awk '/=>/ {print $3}' | grep -v '^$' | sort -u); do
    [[ -f "${lib}" ]] || continue
    base="$(basename "${lib}")"
    case "${base}" in
      libc.so*|libm.so*|libpthread.so*|libdl.so*|librt.so*|ld-linux*|libgcc_s.so*|libstdc++.so*) continue ;;
      libGL*|libEGL*|libGLX*|libOpenGL*|libGLdispatch*|libX11*|libxcb*|libXau*|libXdmcp*|libwayland*) continue ;;
    esac
    case "${lib}" in
      /usr/lib/*|/lib/*|/usr/lib64/*|/lib64/*)
        case "${base}" in
          libSDL3.so*) ;;      # ship it anyway
          *) continue ;;
        esac
        ;;
    esac
    CANDIDATES="${CANDIDATES} ${lib}"
  done

  if [[ -z "${CANDIDATES// /}" ]]; then
    # A fully static SDL3 (the retcomm toolchain pack builds one) leaves
    # nothing to bundle. That is a finished zip, not a skipped step.
    echo "  nothing to bundle — every remaining dependency is a system library"
    echo "Runtime libraries staged in ${STAGE}"
    exit 0
  fi

  # $ORIGIN in the RUNPATH is what makes a lib beside the executable findable.
  # Without it, copying one in is theatre, so say so rather than shipping it.
  # Only demanded when there is actually something to bundle.
  if command -v readelf >/dev/null 2>&1; then
    if ! readelf -d "${EXE}" 2>/dev/null | grep -E 'RUNPATH|RPATH' | grep -q '\$ORIGIN'; then
      echo "::error::$(basename "${EXE}") needs these libraries bundled, but has no \$ORIGIN RUNPATH," >&2
      echo "  so the loader would never find them:" >&2
      printf '    %s\n' ${CANDIDATES} >&2
      echo "  Configure the build with -DCMAKE_BUILD_RPATH='\$ORIGIN'" >&2
      exit 1
    fi
  else
    echo "warning: readelf missing; cannot confirm \$ORIGIN RUNPATH" >&2
  fi

  for lib in ${CANDIDATES}; do
    copy_lib "${lib}"
  done
  ;;
esac

echo "Runtime libraries staged in ${STAGE}"
