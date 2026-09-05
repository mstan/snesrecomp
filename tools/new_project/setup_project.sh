#!/usr/bin/env sh
# New SNES recomp project scaffolding (Linux / macOS / WSL / Git Bash).
#
# On a terminal every setting below is PROMPTED, with the probed ROM identity
# supplying the defaults. Flags are for scripting: anything passed explicitly
# skips its question.
#
# Required:
#   --rom <file.sfc>      Your legally-owned ROM. Probed for identity; never
#                         copied into the repo. A bare path works too
#                         (`setup_project.sh game.sfc`), and on a terminal
#                         with no ROM given, it is the first question.
#
# Common:
#   --dir <parent>        Parent directory for the new repo (default: .)
#   --name <title>        Display name (default: from the dump filename, or
#                         the cartridge header when that reads better)
#   --players <1-8>       Seats. >2 configures a Super Multitap.
#   --multitap <port1|port2|both|off>   Override the players-derived default.
#   --zip-prefix <slug>   Release zip prefix (default: derived from the name)
#   --description <text>  One-line pitch for the README
#   --publisher <name>    README metadata
#   --year <yyyy>         README metadata
#   --region <code>       Default: from the cartridge header
#   --github-owner <org>  Default: TechnicallyComputers
#   --github-repo <name>  Default: the project name
#
# Always on (no flag): mod packages. Every project builds the loader, the
#   launcher's Mods page and the netplay mod-set gate, and ships an (initially
#   empty) mods/preloaded catalog. A build without them cannot exchange mods
#   with a peer, so it is not something a title gets to skip.
#
# Toggles (each has a --no- form):
#   --netplay / --no-netplay        recomp-net delay-sync (default: off)
#   --rollback / --no-rollback      retcomm-rbengine rollback (implies netplay)
#   --ci / --no-ci                  .github/workflows/release.yml (default: on)
#   --fetch-boxart / --no-fetch-boxart
#                                   libretro Named_Boxarts art for the launcher
#                                   (needs the network; asked on a terminal,
#                                   off when non-interactive)
#   --recomp-ui / --no-recomp-ui    Dear ImGui pre-boot launcher: ROM picker,
#                                   verification, display/audio/input settings
#                                   (default: on). Without it the host still
#                                   resolves a ROM, just in text mode.
#   --no-submodules                 skip submodule add/init (offline scaffold;
#                                   the project will not build until you run
#                                   git submodule update --init --recursive)
#   --generate / --no-generate      run tools/regen.sh (default: off; needs the ROM)
#   --build / --no-build            cmake configure + build after generate
#   --create-github / --no-github   gh repo create + push (default: off)
#   --github-visibility <public|private|internal>   default: private
#
# Framework refs:
#   --snesrecomp-ref <ref>    default: main
#   --recomp-ui-ref <ref>     default: the branch this framework checkout
#                             declares in tools/new_project/RECOMP_UI_REF --
#                             its lobby client compiles against recomp-ui's
#                             API, so the framework, not this script, says
#                             which recomp-ui it needs
#   --recomp-net-ref <ref>    override the nested pin inside snesrecomp
#   --rbengine-ref <ref>      override the nested pin inside snesrecomp
#   --snesrecomp-url / --recomp-ui-url
#
# Non-interactive: --yes (or SNESRECOMP_SETUP_YES=1), and any non-TTY run.
#   Every prompt takes its default; toggles stay off unless flagged.
#
# Publish order matches psxrecomp's, and for the same reason: scaffold + CI
# -> commit -> gh repo create (no push) -> generate/build -> one push. An
# early push produces a second "initial" commit that collides on re-run.
#
# Usage:
#   sh tools/new_project/setup_project.sh ~/roms/game.sfc
#   sh tools/new_project/setup_project.sh --rom game.sfc --dir ~/src --yes
#   sh tools/new_project/setup_project.sh            # asks for the ROM
# Windows: powershell -File tools\new_project\setup_project.ps1 -Rom game.sfc
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
TEMPLATE_DIR="$SCRIPT_DIR/templates"
FRAMEWORK_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
FILL_TOKENS="$SCRIPT_DIR/fill_tokens.py"
PROBE_ROM="$SCRIPT_DIR/probe_rom.py"

PYTHON=${PYTHON:-$(command -v python3 || command -v python || true)}
[ -n "$PYTHON" ] || { echo "setup_project: python3 not found on PATH" >&2; exit 1; }

usage() { sed -n '2,60p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

is_tty() { [ -t 0 ] && [ -t 1 ]; }

# Ask for a value, offering a default. Reads /dev/tty so the script still
# prompts correctly when its stdout is being piped.
prompt_line() {
    _q=$1; _var=$2; _def=${3:-}
    if [ -n "$_def" ]; then
        printf '%s [%s]: ' "$_q" "$_def" >/dev/tty
    else
        printf '%s: ' "$_q" >/dev/tty
    fi
    read -r _ans </dev/tty || _ans=
    [ -n "$_ans" ] || _ans=$_def
    eval "$_var=\$_ans"
}

prompt_yn() {
    _q=$1; _var=$2; _def=$3
    _hint=y/N
    [ "$_def" = "1" ] && _hint=Y/n
    while :; do
        printf '%s [%s]: ' "$_q" "$_hint" >/dev/tty
        read -r _ans </dev/tty || _ans=
        if [ -z "$_ans" ]; then
            eval "$_var=$_def"
            return 0
        fi
        case "$_ans" in
            y|Y|yes|YES) eval "$_var=1"; return 0 ;;
            n|N|no|NO)   eval "$_var=0"; return 0 ;;
            *) printf '  please answer y or n\n' >/dev/tty ;;
        esac
    done
}

ROM=""; PARENT="."; NAME=""; PLAYERS=""; MULTITAP=""; ZIP_PREFIX=""
DESCRIPTION=""; PUBLISHER=""; YEAR=""; REGION_OVERRIDE=""
SET_NETPLAY=0; SET_ROLLBACK=0; SET_CI=0; SET_RECOMP_UI=0
SET_GENERATE=0; SET_BUILD=0; SET_GITHUB=0
GITHUB_OWNER="TechnicallyComputers"; GITHUB_REPO=""
ENABLE_NETPLAY=0; ENABLE_ROLLBACK=0; ENABLE_CI=1; ENABLE_RECOMP_UI=1
FETCH_BOXART=0; SET_BOXART=0
ADD_SUBMODULES=1
DO_GENERATE=0; DO_BUILD=0; CREATE_GITHUB=0; GITHUB_VISIBILITY="private"
# Default the framework ref to the branch this checkout is on, for the same
# reason the URL is derived: a scaffold cut from a checkout should pin the
# framework that checkout actually has. Hard-coding "main" silently produced
# projects that could not generate or build whenever the work lived on a
# branch.
SNESRECOMP_REF=$(git -C "$FRAMEWORK_ROOT" symbolic-ref --quiet --short HEAD 2>/dev/null || true)
[ -n "$SNESRECOMP_REF" ] || SNESRECOMP_REF="main"
# recomp-ui is not a submodule of the framework, but the framework's lobby
# client is compiled against its header, so the framework declares the ref
# it needs (tools/new_project/RECOMP_UI_REF) and a pin bump is one edit there.
# "master" was hard-coded here once, and every netplay project cut with the
# default failed to compile snes_host_lobby.c against a recomp-ui that had
# never heard of the lobby mod-transfer callbacks.
RECOMP_UI_REF=$(sed -n '1{s/[[:space:]]*$//;p}' "$SCRIPT_DIR/RECOMP_UI_REF" 2>/dev/null || true)
[ -n "$RECOMP_UI_REF" ] || RECOMP_UI_REF="master"
RECOMP_NET_REF=""; RBENGINE_REF=""
# The framework URL comes from the checkout this script is running out of, so
# it cannot drift from where snesrecomp actually lives. (It was hard-coded to
# the wrong org once; deriving it removes the class.)
SNESRECOMP_URL=$(git -C "$FRAMEWORK_ROOT" remote get-url origin 2>/dev/null || true)
[ -n "$SNESRECOMP_URL" ] || SNESRECOMP_URL="https://github.com/mstan/snesrecomp.git"
RECOMP_UI_URL="https://github.com/mstan/recomp-ui.git"
DEFAULT_BRANCH="main"
ASSUME_YES=${SNESRECOMP_SETUP_YES:-0}

while [ $# -gt 0 ]; do
    case "$1" in
        --rom) ROM=$2; shift 2 ;;
        --dir) PARENT=$2; shift 2 ;;
        --name) NAME=$2; shift 2 ;;
        --players) PLAYERS=$2; shift 2 ;;
        --multitap) MULTITAP=$2; shift 2 ;;
        --zip-prefix) ZIP_PREFIX=$2; shift 2 ;;
        --github-owner) GITHUB_OWNER=$2; shift 2 ;;
        --github-repo) GITHUB_REPO=$2; shift 2 ;;
        --description) DESCRIPTION=$2; shift 2 ;;
        --publisher) PUBLISHER=$2; shift 2 ;;
        --year) YEAR=$2; shift 2 ;;
        --region) REGION_OVERRIDE=$2; shift 2 ;;
        --netplay) ENABLE_NETPLAY=1; SET_NETPLAY=1; shift ;;
        --no-netplay) ENABLE_NETPLAY=0; ENABLE_ROLLBACK=0; SET_NETPLAY=1; SET_ROLLBACK=1; shift ;;
        --rollback) ENABLE_ROLLBACK=1; ENABLE_NETPLAY=1; SET_NETPLAY=1; SET_ROLLBACK=1; shift ;;
        --no-rollback) ENABLE_ROLLBACK=0; SET_ROLLBACK=1; shift ;;
        --recomp-ui) ENABLE_RECOMP_UI=1; SET_RECOMP_UI=1; shift ;;
        --no-recomp-ui) ENABLE_RECOMP_UI=0; SET_RECOMP_UI=1; shift ;;
        --ci) ENABLE_CI=1; SET_CI=1; shift ;;
        --fetch-boxart) FETCH_BOXART=1; SET_BOXART=1; shift ;;
        --no-fetch-boxart) FETCH_BOXART=0; SET_BOXART=1; shift ;;
        --no-submodules) ADD_SUBMODULES=0; shift ;;
        --no-ci) ENABLE_CI=0; SET_CI=1; shift ;;
        --generate) DO_GENERATE=1; SET_GENERATE=1; shift ;;
        --no-generate) DO_GENERATE=0; SET_GENERATE=1; shift ;;
        --build) DO_BUILD=1; DO_GENERATE=1; SET_BUILD=1; SET_GENERATE=1; shift ;;
        --no-build) DO_BUILD=0; SET_BUILD=1; shift ;;
        --create-github) CREATE_GITHUB=1; SET_GITHUB=1; shift ;;
        --no-github) CREATE_GITHUB=0; SET_GITHUB=1; shift ;;
        --github-visibility) GITHUB_VISIBILITY=$2; shift 2 ;;
        --snesrecomp-ref) SNESRECOMP_REF=$2; shift 2 ;;
        --recomp-ui-ref) RECOMP_UI_REF=$2; shift 2 ;;
        --recomp-net-ref) RECOMP_NET_REF=$2; shift 2 ;;
        --rbengine-ref) RBENGINE_REF=$2; shift 2 ;;
        --snesrecomp-url) SNESRECOMP_URL=$2; shift 2 ;;
        --recomp-ui-url) RECOMP_UI_URL=$2; shift 2 ;;
        --yes|-y) ASSUME_YES=1; shift ;;
        -h|--help) usage 0 ;;
        -*) echo "setup_project: unknown flag: $1" >&2; usage 2 ;;
        *)  # A bare argument is the ROM: `setup_project.sh game.sfc`.
            if [ -n "$ROM" ]; then
                echo "setup_project: two ROMs given ('$ROM' and '$1');" >&2
                echo "               a project is cut from exactly one image." >&2
                exit 2
            fi
            ROM=$1; shift ;;
    esac
done

# No ROM on the command line: on a terminal it is simply the first question.
# Anything else (scripts, CI, --yes) has to say what it means.
if [ -z "$ROM" ]; then
    if [ "$ASSUME_YES" != "1" ] && is_tty; then
        echo "A new project is cut from one legally-owned ROM (.sfc/.smc). It is"
        echo "probed here and never copied into the repository."
        while :; do
            prompt_line "Path to the ROM" ROM ""
            # Shells and file managers hand over quoted or ~-prefixed paths.
            ROM=$(printf '%s' "$ROM" | sed -e "s/^['\"]//" -e "s/['\"]\$//")
            case "$ROM" in "~/"*) ROM="$HOME/${ROM#\~/}" ;; esac
            [ -n "$ROM" ] || { echo "setup_project: a ROM is required" >&2; exit 2; }
            [ -f "$ROM" ] && break
            printf '  not found: %s\n' "$ROM" >/dev/tty
        done
    else
        echo "setup_project: --rom <file.sfc> (or a bare path) is required" >&2
        usage 2
    fi
fi
[ -f "$ROM" ] || { echo "setup_project: ROM not found: $ROM" >&2; exit 1; }
ROM_ABS=$(CDPATH= cd -- "$(dirname -- "$ROM")" && pwd)/$(basename -- "$ROM")

# ── Probe the ROM ─────────────────────────────────────────────────────────
echo "== Probing ROM =="
PROBE_JSON=$(mktemp)
trap 'rm -f "$PROBE_JSON"' EXIT   # replaced by cleanup_partial once ROOT exists
"$PYTHON" "$PROBE_ROM" "$ROM_ABS" --json-out "$PROBE_JSON" --quiet

probe_get() {
    "$PYTHON" -c 'import json,sys; print(json.load(open(sys.argv[1]))[sys.argv[2]])' \
        "$PROBE_JSON" "$1"
}

SUGGESTED_NAME=$(probe_get display_name)
HEADER_TITLE=$(probe_get header_title)
ROM_FILE=$(basename -- "$ROM_ABS")
ROM_MAPPING=$(probe_get mapping)
ROM_CRC32=$(probe_get crc32)
ROM_SHA256=$(probe_get sha256)
REGION=$(probe_get region)
REGION_NAME=$(probe_get region_name)
COPROCESSOR=$(probe_get coprocessor)
CHECKSUM_OK=$(probe_get checksum_valid)
ROM_SIZE_KB=$(probe_get rom_size_kb)
SRAM_SIZE_KB=$(probe_get sram_size_kb)
SETUP_DATE=$(date -u +%Y-%m-%d)

echo "  title (header):   $HEADER_TITLE"
echo "  mapping / region: $ROM_MAPPING / $REGION ($REGION_NAME)"
echo "  coprocessor:      $COPROCESSOR"
echo "  size:             ${ROM_SIZE_KB} KiB ROM, ${SRAM_SIZE_KB} KiB SRAM"
echo "  crc32 / sha256:   $ROM_CRC32 / $ROM_SHA256"

if [ "$CHECKSUM_OK" != "True" ]; then
    echo "warning: cartridge header checksum does not verify — the image may" >&2
    echo "         be modified, over-dumped, or not a plain SNES ROM." >&2
fi
if [ "$COPROCESSOR" = "unknown" ]; then
    echo "warning: unrecognised cartridge type — check coprocessor support" >&2
    echo "         before investing in this port." >&2
fi

# ── Questions ─────────────────────────────────────────────────────────────
# Everything below is asked on a terminal, defaulted from the probe. A flag
# passed explicitly skips its question; --yes and non-TTY take every default.
INTERACTIVE=0
if [ "$ASSUME_YES" != "1" ] && is_tty; then INTERACTIVE=1; fi

if [ -z "$NAME" ]; then
    if [ "$INTERACTIVE" -eq 1 ]; then
        echo
        prompt_line "Display name" NAME "$SUGGESTED_NAME"
    else
        NAME="$SUGGESTED_NAME"
    fi
fi
[ -n "$NAME" ] || { echo "setup_project: a display name is required" >&2; exit 1; }

if [ -z "$PLAYERS" ]; then
    if [ "$INTERACTIVE" -eq 1 ]; then
        prompt_line "Max players (1-8)" PLAYERS "2"
    else
        PLAYERS=2
    fi
fi
case "$PLAYERS" in
    ''|*[!0-9]*) echo "setup_project: players must be 1-8 (got: $PLAYERS)" >&2; exit 2 ;;
esac
[ "$PLAYERS" -ge 1 ] && [ "$PLAYERS" -le 8 ] || {
    echo "setup_project: players must be 1-8 (got: $PLAYERS)" >&2; exit 2; }

# Seats above two need a tap. Two taps only above five — the port-2 tap is the
# five-player layout every commercial title uses (snesrecomp/docs/MULTITAP.md).
if [ -z "$MULTITAP" ]; then
    if [ "$PLAYERS" -gt 5 ]; then MULTITAP=both
    elif [ "$PLAYERS" -gt 2 ]; then MULTITAP=port2
    else MULTITAP=off
    fi
    if [ "$INTERACTIVE" -eq 1 ] && [ "$MULTITAP" != off ]; then
        prompt_line "Multitap (port1/port2/both)" MULTITAP "$MULTITAP"
    fi
fi
case "$MULTITAP" in
    off|port1|port2|both) ;;
    *) echo "setup_project: --multitap must be port1, port2, both, or off" >&2; exit 2 ;;
esac

PROJECT_NAME=$("$PYTHON" -c '
import sys; sys.path.insert(0, sys.argv[1])
from probe_rom import project_name; print(project_name(sys.argv[2]))' \
    "$SCRIPT_DIR" "$NAME")
ROM_SLUG=$("$PYTHON" -c '
import sys; sys.path.insert(0, sys.argv[1])
from probe_rom import safe_slug; print(safe_slug(sys.argv[2]).lower())' \
    "$SCRIPT_DIR" "$NAME")

if [ -z "$ZIP_PREFIX" ]; then
    if [ "$INTERACTIVE" -eq 1 ]; then
        prompt_line "Release zip / CI artifact prefix" ZIP_PREFIX "$ROM_SLUG"
    else
        ZIP_PREFIX="$ROM_SLUG"
    fi
fi

if [ "$INTERACTIVE" -eq 1 ]; then
    [ -n "$DESCRIPTION" ] || prompt_line "Short description (optional)" DESCRIPTION ""
    [ -n "$PUBLISHER" ] || prompt_line "Publisher (optional)" PUBLISHER ""
    [ -n "$YEAR" ] || prompt_line "Release year (optional)" YEAR ""
    [ -n "$REGION_OVERRIDE" ] || prompt_line "Region" REGION_OVERRIDE "$REGION"
fi
[ -z "$REGION_OVERRIDE" ] || REGION="$REGION_OVERRIDE"
# The id a mod package's [[target]] names (MOD_PACKAGES.md), e.g. gwed-jp.
# Derived once, recorded in rom_identity.txt, compiled in from there.
GAME_ID="$ROM_SLUG-$(printf '%s' "$REGION" | tr 'A-Z' 'a-z')"

if [ "$SET_RECOMP_UI" -eq 0 ] && [ "$INTERACTIVE" -eq 1 ]; then
    prompt_yn "Include the recomp-ui launcher submodule?" ENABLE_RECOMP_UI 1
fi

# Boxart is a network fetch, so it is asked rather than assumed, and stays off
# for a non-interactive run unless --fetch-boxart says otherwise -- a script
# or a test should not reach the internet because a default said so.
if [ "$ENABLE_RECOMP_UI" -eq 1 ] && [ "$SET_BOXART" -eq 0 ] && [ "$INTERACTIVE" -eq 1 ]; then
    prompt_yn "Fetch libretro boxart for the launcher now? (needs the network)" FETCH_BOXART 1
fi

if [ "$PLAYERS" -eq 1 ]; then
    if [ "$ENABLE_NETPLAY" -eq 1 ]; then
        echo "warning: netplay ignored for a 1-player title." >&2
    fi
    ENABLE_NETPLAY=0; ENABLE_ROLLBACK=0
    SET_NETPLAY=1; SET_ROLLBACK=1
elif [ "$SET_NETPLAY" -eq 0 ] && [ "$INTERACTIVE" -eq 1 ]; then
    prompt_yn "Enable netplay (recomp-net delay-sync)?" ENABLE_NETPLAY 0
fi

if [ "$ENABLE_NETPLAY" -eq 1 ]; then
    if [ "$SET_ROLLBACK" -eq 0 ] && [ "$INTERACTIVE" -eq 1 ]; then
        prompt_yn "Also build rollback (retcomm-rbengine)?" ENABLE_ROLLBACK 1
    fi
elif [ "$ENABLE_ROLLBACK" -eq 1 ]; then
    echo "warning: rollback needs netplay — enabling netplay." >&2
    ENABLE_NETPLAY=1
fi

if [ "$SET_CI" -eq 0 ] && [ "$INTERACTIVE" -eq 1 ]; then
    prompt_yn "Add the GitHub Actions workflow?" ENABLE_CI 1
fi

if [ "$SET_GENERATE" -eq 0 ] && [ "$INTERACTIVE" -eq 1 ]; then
    prompt_yn "Generate C from the ROM now?" DO_GENERATE 1
fi
if [ "$DO_GENERATE" -eq 1 ]; then
    if [ "$SET_BUILD" -eq 0 ] && [ "$INTERACTIVE" -eq 1 ]; then
        prompt_yn "Configure and build after generating?" DO_BUILD 1
    fi
elif [ "$DO_BUILD" -eq 1 ]; then
    echo "warning: build needs generate — enabling generate." >&2
    DO_GENERATE=1
fi

# GitHub is opt-in and asked plainly: nothing here assumes a repo exists, and
# declining skips creation, the remote, and the push entirely.
if [ "$SET_GITHUB" -eq 0 ] && [ "$INTERACTIVE" -eq 1 ]; then
    prompt_yn "Create a GitHub repo for this project with gh?" CREATE_GITHUB 0
fi
if [ "$CREATE_GITHUB" -eq 1 ]; then
    if [ "$INTERACTIVE" -eq 1 ]; then
        prompt_line "GitHub owner / org" GITHUB_OWNER "$GITHUB_OWNER"
        prompt_line "GitHub repo name" GITHUB_REPO "${GITHUB_REPO:-$PROJECT_NAME}"
        prompt_line "Visibility (public/private/internal)" GITHUB_VISIBILITY \
            "$GITHUB_VISIBILITY"
    fi
    case "$GITHUB_VISIBILITY" in
        public|private|internal) ;;
        *) echo "setup_project: visibility must be public, private, or internal" >&2
           exit 2 ;;
    esac
fi
[ -n "$GITHUB_REPO" ] || GITHUB_REPO="$PROJECT_NAME"

ROOT="$PARENT/$PROJECT_NAME"
if [ -e "$ROOT" ]; then
    echo "setup_project: $ROOT already exists — refusing to overwrite" >&2
    exit 1
fi

if [ "$PLAYERS" -le 1 ]; then
    PLAYERS_NOTE="Single player."
elif [ "$MULTITAP" = "off" ]; then
    PLAYERS_NOTE="Two players, one controller per port."
else
    PLAYERS_NOTE="Up to $PLAYERS players through a Super Multitap ($MULTITAP). Seats
beyond the second are driven with \`RtlSetPadState\` — see
\`snesrecomp/docs/MULTITAP.md\`."
fi
if [ "$ENABLE_ROLLBACK" -eq 1 ]; then
    PLAYERS_NOTE="$PLAYERS_NOTE

Netplay is built with rollback available (\`SNES_NET_MODE=rollback\`); delay-sync
remains the default. See \`snesrecomp/docs/ROLLBACK.md\`."
elif [ "$ENABLE_NETPLAY" -eq 1 ]; then
    PLAYERS_NOTE="$PLAYERS_NOTE

Netplay is built with delay-sync. See \`snesrecomp/docs/RECOMP_NET.md\`."
fi

NETPLAY_BLOCK="# Netplay is not built into this target."
if [ "$ENABLE_NETPLAY" -eq 1 ]; then
    NETPLAY_BLOCK="snesrecomp_enable_recomp_net($PROJECT_NAME)"
fi
if [ "$ENABLE_ROLLBACK" -eq 1 ]; then
    NETPLAY_BLOCK="$NETPLAY_BLOCK
snesrecomp_enable_rollback($PROJECT_NAME)"
fi
# The pre-boot GUI launcher lives in recomp-ui, so it is wired only when that
# submodule is present. Without it the host still resolves a ROM (positional ->
# beside the exe -> rom.cfg -> native picker) — main.c compiles the GUI blocks
# out on RECOMP_LAUNCHER, which this call is what defines.
LAUNCHER_BLOCK="# No GUI launcher: this project was scaffolded with --no-recomp-ui.
# Add the submodule and re-run this block to get one:
#   git submodule add https://github.com/mstan/recomp-ui.git recomp-ui
#   set(RECOMP_UI_ROOT \"\${CMAKE_SOURCE_DIR}/recomp-ui\" CACHE PATH \"\" FORCE)
#   include(\${RECOMP_UI_ROOT}/recomp_ui.cmake)
#   recomp_target_launcher_ui($PROJECT_NAME CONSOLE snes)"
if [ "$ENABLE_RECOMP_UI" -eq 1 ]; then
    LAUNCHER_BLOCK="# Shared Dear ImGui pre-boot launcher (ROM picker, verification, display /
# audio / input settings). Self-contained: it brings its own ImGui, GL link and
# staged assets, and defines RECOMP_LAUNCHER so src/main.c compiles its GUI path.
set(RECOMP_UI_ROOT \"\${CMAKE_SOURCE_DIR}/recomp-ui\" CACHE PATH
    \"Root directory of the recomp-ui launcher repo\" FORCE)
if(NOT EXISTS \"\${RECOMP_UI_ROOT}/recomp_ui.cmake\")
    message(FATAL_ERROR
        \"recomp-ui missing at \${RECOMP_UI_ROOT}.\\n\"
        \"Run: git submodule update --init --recursive recomp-ui\")
endif()
include(\${RECOMP_UI_ROOT}/recomp_ui.cmake)
# BOXART stages launcher_assets/img/boxart.tga beside the exe; the launcher
# shows assets/img/boxart.tga by default. EXISTS-guarded in recomp_ui.cmake,
# so this configures cleanly before any art is fetched.
recomp_target_launcher_ui($PROJECT_NAME CONSOLE snes
    BOXART \"\${CMAKE_SOURCE_DIR}/launcher_assets/img/boxart.tga\")

# Generate & rebuild from inside the launcher: pick ROM -> recompile locally
# -> cmake --build -> relaunch. The implementation is the framework's; this
# project contributes no configuration (snesrecomp_codegen_host_autowire in
# src/main.c). recomp-ui/src is on the include path because the host speaks
# RecompLauncherCGameInfo.
target_sources($PROJECT_NAME PRIVATE
    \${SNESRECOMP_ROOT}/host/snesrecomp_codegen_host.c)
target_include_directories($PROJECT_NAME PRIVATE
    \${SNESRECOMP_ROOT}/host
    \${RECOMP_UI_ROOT}/src)"
fi

MULTITAP_BLOCK="# No multitap: two seats, one controller per port."
if [ "$MULTITAP" != "off" ]; then
    MULTITAP_BLOCK="# Seats: $PLAYERS. Call RtlSetMultitap() at startup, or launch with
# SNES_MULTITAP=$MULTITAP. See snesrecomp/docs/MULTITAP.md."
fi

# README metadata: blanks read badly in a table, so show an em dash.
PUBLISHER_DISP=${PUBLISHER:-—}
YEAR_DISP=${YEAR:-—}
DESCRIPTION_MD="$DESCRIPTION"
[ -n "$DESCRIPTION_MD" ] || DESCRIPTION_MD="_Add a short description here._"

echo "== New SNES project =="
echo "  repo:       $ROOT"
echo "  title:      $NAME"
echo "  rom:        $ROM_FILE ($ROM_MAPPING, $REGION, crc32 $ROM_CRC32)"
echo "  zip prefix: $ZIP_PREFIX"
echo "  players:    $PLAYERS (multitap: $MULTITAP)"
echo "  netplay:    $ENABLE_NETPLAY (rollback: $ENABLE_ROLLBACK)"
if [ "$ENABLE_RECOMP_UI" -eq 1 ]; then
    echo "  recomp-ui:  $RECOMP_UI_REF (boxart: $FETCH_BOXART)"
else
    echo "  recomp-ui:  no (text-mode host)"
fi
echo "  mods:       on (always: loader, Mods page, netplay mod-set gate)"
echo "  CI:         $ENABLE_CI"
echo "  generate:   $DO_GENERATE (build: $DO_BUILD)"
if [ "$CREATE_GITHUB" -eq 1 ]; then
    echo "  github:     create $GITHUB_OWNER/$GITHUB_REPO ($GITHUB_VISIBILITY)"
else
    echo "  github:     no (local repo only)"
fi

if [ "$ASSUME_YES" != "1" ] && [ -t 0 ] && [ -t 1 ]; then
    printf 'Proceed? [Y/n]: '
    read _ans </dev/tty || _ans=
    case "${_ans:-y}" in n|N|no|NO) echo "aborted"; exit 0 ;; esac
fi

# ── Preflight ─────────────────────────────────────────────────────────────
# Check the submodule remotes BEFORE creating anything. A URL that resolves to
# nothing used to fail half way through and leave a partial project directory
# behind, which is a worse outcome than not starting.
FRAMEWORK_REF_UNPUSHED=0
if [ "$ADD_SUBMODULES" -eq 1 ]; then
    echo "== Checking framework remotes =="
    for _pair in "snesrecomp|$SNESRECOMP_URL" "recomp-ui|$RECOMP_UI_URL"; do
        _what=${_pair%%|*}
        _url=${_pair#*|}
        [ "$_what" = "recomp-ui" ] && [ "$ENABLE_RECOMP_UI" -eq 0 ] && continue
        case "$_url" in
            /*|file://*) continue ;;   # local path: nothing to reach
        esac
        if git ls-remote --exit-code "$_url" HEAD >/dev/null 2>&1; then
            echo "  ok  $_what -> $_url"
            if [ "$_what" = "snesrecomp" ] &&
               ! git ls-remote --exit-code --heads "$_url" "$SNESRECOMP_REF" \
                   >/dev/null 2>&1; then
                echo "warning: the framework branch '$SNESRECOMP_REF' is not on" >&2
                echo "         $_url yet." >&2
                echo "         The scaffold will pin the commit you have locally," >&2
                echo "         so it builds here but nobody else can clone it" >&2
                echo "         until that branch is pushed." >&2
                FRAMEWORK_REF_UNPUSHED=1
            fi
        else
            echo "setup_project: cannot reach the $_what remote:" >&2
            echo "  $_url" >&2
            echo "Check the URL (--${_what}-url), your network, and that you" >&2
            echo "have access to it. Nothing has been created." >&2
            exit 1
        fi
    done
fi

if [ "$CREATE_GITHUB" -eq 1 ]; then
    if ! command -v gh >/dev/null 2>&1; then
        echo "setup_project: --create-github needs the gh CLI, which is not" >&2
        echo "installed. Install it, or re-run without creating a repo." >&2
        exit 1
    fi
    if ! gh auth status >/dev/null 2>&1; then
        echo "setup_project: gh is not authenticated. Run 'gh auth login'," >&2
        echo "or re-run without creating a repo. Nothing has been created." >&2
        exit 1
    fi
    echo "  ok  gh authenticated"
fi

# ── Scaffold ──────────────────────────────────────────────────────────────
echo "== Creating layout =="
mkdir -p "$ROOT"
ROOT=$(CDPATH= cd -- "$ROOT" && pwd)

# From here on, a failure removes the directory we created rather than leaving
# a half-built project that the next run then refuses to overwrite. Cleared
# once the scaffold is committed and re-running is safe.
SCAFFOLD_INCOMPLETE=1
cleanup_partial() {
    _rc=$?
    rm -f "$PROBE_JSON"
    if [ "$_rc" -ne 0 ] && [ "${SCAFFOLD_INCOMPLETE:-0}" -eq 1 ] && [ -n "$ROOT" ]; then
        cd /
        rm -rf "$ROOT"
        echo "setup_project: failed — removed the partial project at $ROOT" >&2
    fi
}
trap cleanup_partial EXIT

cd "$ROOT"
mkdir -p recomp src src/gen tools scripts assets .github/workflows \
         mods/preloaded/packages
git init -q -b "$DEFAULT_BRANCH" .

fill() {
    "$PYTHON" "$FILL_TOKENS" "$TEMPLATE_DIR/$1" "$ROOT/$2" \
        --set "PROJECT_NAME=$PROJECT_NAME" \
        --set "DISPLAY_NAME=$NAME" \
        --set "WINDOW_TITLE=$NAME" \
        --set "CATALOG_SLUG=$ROM_SLUG" \
        --set "ROM_FILE=$ROM_FILE" \
        --set "ROM_SLUG=$ROM_SLUG" \
        --set "ROM_MAPPING=$ROM_MAPPING" \
        --set "ROM_CRC32=$ROM_CRC32" \
        --set "ROM_SHA256=$ROM_SHA256" \
        --set "REGION=$REGION" \
        --set "REGION_NAME=$REGION_NAME" \
        --set "GAME_ID=$GAME_ID" \
        --set "COPROCESSOR=$COPROCESSOR" \
        --set "ZIP_PREFIX=$ZIP_PREFIX" \
        --set "PLAYERS=$PLAYERS" \
        --set "PLAYERS_NOTE=$PLAYERS_NOTE" \
        --set "NETPLAY_BLOCK=$NETPLAY_BLOCK" \
        --set "LAUNCHER_BLOCK=$LAUNCHER_BLOCK" \
        --set "MULTITAP_BLOCK=$MULTITAP_BLOCK" \
        --set "SETUP_DATE=$SETUP_DATE" \
        --set "DESCRIPTION=$DESCRIPTION_MD" \
        --set "PUBLISHER=$PUBLISHER_DISP" \
        --set "YEAR=$YEAR_DISP" \
        --set "GITHUB_OWNER=$GITHUB_OWNER" \
        --set "GITHUB_REPO=$GITHUB_REPO"
}

fill CMakeLists.txt.in   CMakeLists.txt
fill VERSION.in          VERSION
fill gitignore.in        .gitignore
fill README.md.in        README.md
fill main.c.in           src/main.c
fill game_rtl.c.in       src/game_rtl.c
fill game_rtl.h.in       src/game_rtl.h
fill gen_stubs.c.in      src/gen_stubs.c
fill variables.h.in      src/variables.h
fill host_contract.c.in  src/host_contract.c
fill rom_identity.txt.in rom_identity.txt
fill regen.sh.in         tools/regen.sh
fill package_release.sh.in scripts/package_release.sh
fill symbols_readme.md.in  recomp/README.md
chmod +x tools/regen.sh scripts/package_release.sh

# Empty mod catalog. The build stages mods/ beside the executable on every
# build of it, and the runtime initializes from mods/preloaded there; an empty
# catalog is a valid one (the Mods page just lists nothing).
cat > mods/preloaded/README.md <<EOF
# Preloaded mods

Ship reviewed, default-disabled packages here:

\`\`\`text
packages/<package-id>/<version>/
  manifest.toml
  ...
\`\`\`

A manifest's \`[[target]]\` names this title as \`game_id = "$GAME_ID"\` with the
ROM's SHA-256 (both live in \`rom_identity.txt\`). The build copies \`mods/\`
beside the executable on every build; nothing placed there by hand survives.
Players install \`.snesmod\` archives through the launcher's Mods page, which
the runtime keeps under its own state beside the executable.

See \`snesrecomp/docs/MOD_PACKAGES.md\` for the manifest format and the
trusted-plugin registration a package can activate.
EOF
: > mods/preloaded/packages/.gitkeep
: > src/gen/.gitkeep

echo "== Seeding analysis config =="
"$PYTHON" "$PROBE_ROM" "$ROM_ABS" --quiet --display-name "$NAME" \
    --write-seed-cfg "$ROOT/recomp/bank00.cfg" \
    --write-symbols "$ROOT/recomp/symbols.toml"
cp "$FRAMEWORK_ROOT/LICENSE" "$ROOT/LICENSE" 2>/dev/null || true

if [ "$FETCH_BOXART" -eq 1 ] && [ "$ENABLE_RECOMP_UI" -eq 1 ]; then
    echo "== Fetching boxart (libretro Named_Boxarts) =="
    # Same flow as psxrecomp's wizard: TGA for the launcher + PNG for the
    # README, sourced and attributed in BOXART_SOURCE.txt. A miss is a warning
    # — CMake already carries the BOXART argument and stages the file the
    # moment it exists.
    ROM_STEM=${ROM_FILE%.*}
    if "$PYTHON" "$SCRIPT_DIR/fetch_boxart.py" \
        --out "$ROOT/launcher_assets/img/boxart.tga" \
        --cue-stem "$ROM_STEM" \
        --display-name "$NAME"; then
        :
    else
        echo "warning: boxart fetch failed — launcher shows no art until" \
             "launcher_assets/img/boxart.tga exists." >&2
    fi
fi

if [ "$ENABLE_CI" -eq 1 ]; then
    echo "== CI workflow =="
    fill release.yml.in .github/workflows/release.yml
fi

# ── Submodules ────────────────────────────────────────────────────────────
FRAMEWORK_GAPS=""
if [ "$ADD_SUBMODULES" -eq 0 ]; then
    echo "== Skipping submodules (--no-submodules) =="
    echo "snesrecomp=<not added>" > framework_pins.txt
else
echo "== Adding submodules =="
git submodule add -q -b "$SNESRECOMP_REF" "$SNESRECOMP_URL" snesrecomp 2>/dev/null ||
    git submodule add -q "$SNESRECOMP_URL" snesrecomp
if [ "$FRAMEWORK_REF_UNPUSHED" -eq 1 ]; then
    # Take the commit from the local checkout so generate/build work now.
    # The pin is recorded either way; pushing the branch later makes it
    # resolvable for everyone else without changing this project.
    echo "== Using the local framework commit ($SNESRECOMP_REF) =="
    git -C snesrecomp fetch -q "$FRAMEWORK_ROOT" "$SNESRECOMP_REF"
    git -C snesrecomp checkout --detach -q FETCH_HEAD
    # Record the gitlink NOW. `submodule update` below restores each submodule
    # to whatever the index says, so a checkout that is not staged first gets
    # snapped straight back to the commit `submodule add` recorded.
    git add snesrecomp
fi
if [ "$ENABLE_RECOMP_UI" -eq 1 ]; then
    git submodule add -q -b "$RECOMP_UI_REF" "$RECOMP_UI_URL" recomp-ui
fi
git submodule update --init --recursive

# Nested modules live inside snesrecomp: recomp-net owns the wire and the
# episode FSM, retcomm-rbengine the rollback host policy. The gitlink SHA the
# framework pins is what CI builds; an override here follows a branch instead.
override_nested() {
    _path=$1; _ref=$2
    [ -n "$_ref" ] || return 0
    [ -d "snesrecomp/$_path" ] || return 0
    echo "== Override $_path -> $_ref =="
    git -C "snesrecomp/$_path" fetch -q origin "$_ref"
    git -C "snesrecomp/$_path" checkout --detach -q FETCH_HEAD
    git -C snesrecomp add "$_path"
}
override_nested lib/recomp-net "$RECOMP_NET_REF"
override_nested lib/retcomm-rbengine "$RBENGINE_REF"

git -C snesrecomp checkout --detach -q HEAD
git add snesrecomp
[ "$ENABLE_RECOMP_UI" -eq 1 ] && git -C recomp-ui checkout --detach -q HEAD && git add recomp-ui

echo "== Framework pins =="
{
    echo "snesrecomp=$(git -C snesrecomp rev-parse HEAD)"
    if [ "$ENABLE_RECOMP_UI" -eq 1 ]; then
        echo "recomp-ui=$(git -C recomp-ui rev-parse HEAD)"
    fi
    for _nested in lib/recomp-net lib/retcomm-rbengine; do
        if [ -d "snesrecomp/$_nested/.git" ] || [ -f "snesrecomp/$_nested/.git" ]; then
            echo "$(basename "$_nested")=$(git -C "snesrecomp/$_nested" rev-parse HEAD)"
        fi
    done
} | tee framework_pins.txt

# ── Framework capability check ────────────────────────────────────────────
# Reachable is not the same as capable. The submodule is pinned to a ref, and
# a ref that predates a feature cannot provide it — so check what the pinned
# framework can actually do against what was asked for, here, instead of
# letting it fail later inside regen.sh with an argparse error that names
# nothing useful.
note_gap() { FRAMEWORK_GAPS="$FRAMEWORK_GAPS  - $1
"; }

if [ "$DO_GENERATE" -eq 1 ] &&
   ! "$PYTHON" snesrecomp/snesrecomp_cli.py generate --help >/dev/null 2>&1; then
    note_gap "generate: this framework ref has no 'snesrecomp_cli.py generate'"
    DO_GENERATE=0
    DO_BUILD=0
fi
if [ "$ENABLE_NETPLAY" -eq 1 ] && [ ! -f snesrecomp/lib/recomp-net/CMakeLists.txt ]; then
    note_gap "netplay: snesrecomp/lib/recomp-net is missing"
fi
# The lobby client (runner/src/netplay/snes_host_lobby.c) fills recomp-ui's
# netplay callback table, mod-transfer entries included, whether or not this
# project enables netplay -- so the recomp-ui ref must carry that API. Check
# the header now instead of letting the first build fail on a struct member.
if [ "$ENABLE_NETPLAY" -eq 1 ] && [ "$ENABLE_RECOMP_UI" -eq 1 ] &&
   [ -f recomp-ui/src/recomp_launcher.h ] &&
   ! grep -q 'lobby_mods_can_download' recomp-ui/src/recomp_launcher.h; then
    note_gap "netplay: recomp-ui ref '$RECOMP_UI_REF' lacks the lobby mod-transfer API the framework's lobby client needs (try --recomp-ui-ref $(cat "$SCRIPT_DIR/RECOMP_UI_REF" 2>/dev/null || echo merge/frameblend-localization))"
fi
if [ "$ENABLE_ROLLBACK" -eq 1 ]; then
    if [ ! -f snesrecomp/lib/retcomm-rbengine/CMakeLists.txt ]; then
        note_gap "rollback: snesrecomp/lib/retcomm-rbengine is missing"
    elif ! grep -q "snesrecomp_enable_rollback" snesrecomp/runner/recomp_net.cmake 2>/dev/null; then
        note_gap "rollback: the framework has no snesrecomp_enable_rollback()"
    fi
fi

if [ -n "$FRAMEWORK_GAPS" ]; then
    echo >&2
    echo "warning: the pinned framework ref '$SNESRECOMP_REF' does not support" >&2
    echo "         everything this project asked for:" >&2
    printf '%s' "$FRAMEWORK_GAPS" >&2
    echo "         The scaffold is still correct — it will work as soon as the" >&2
    echo "         framework catches up. Options:" >&2
    echo "           * pick a ref that has these: --snesrecomp-ref <branch>" >&2
    echo "           * point at a local checkout: SNESRECOMP_ROOT=/path/to/snesrecomp" >&2
    echo "           * or commit/push the framework work and re-pin the submodule" >&2
    echo >&2
fi
fi

# ── Commit ────────────────────────────────────────────────────────────────
git add -A
COMMITTED=0
if git -c user.email=setup@localhost -c user.name=setup \
    commit -q -m "Initial $PROJECT_NAME scaffold" 2>/dev/null; then
    COMMITTED=1
    SCAFFOLD_INCOMPLETE=0
    echo "== Committed scaffold =="
else
    SCAFFOLD_INCOMPLETE=0
    echo "  (nothing committed — commit manually when ready)"
fi

# ── GitHub: create now, push after generate/build ─────────────────────────
GITHUB_CREATED=0
GITHUB_RECOVERY=0
if [ "$CREATE_GITHUB" -eq 1 ]; then
    echo "== GitHub repo (create only; push deferred) =="
    if ! command -v gh >/dev/null 2>&1; then
        echo "warning: gh not installed — skipping create." >&2
    elif [ "$COMMITTED" -eq 0 ]; then
        echo "warning: no commit to publish — skipping create." >&2
    else
        VIS=--private
        [ "$GITHUB_VISIBILITY" = public ] && VIS=--public
        [ "$GITHUB_VISIBILITY" = internal ] && VIS=--internal
        if gh repo create "$GITHUB_OWNER/$GITHUB_REPO" $VIS --source="$ROOT" \
            --remote=origin --description "Native recompilation of $NAME"; then
            GITHUB_CREATED=1
        else
            # Usually transient (API blip, rate limit). The scaffold is
            # already committed, so publishing later is two commands — print
            # them rather than leaving the user to reconstruct them.
            GITHUB_RECOVERY=1
            echo "warning: gh repo create failed. The project is committed" >&2
            echo "         locally; publish it later with:" >&2
            echo "           gh repo create $GITHUB_OWNER/$GITHUB_REPO --$GITHUB_VISIBILITY \\" >&2
            echo "             --source '$ROOT' --remote=origin" >&2
            echo "           git -C '$ROOT' push -u origin HEAD" >&2
        fi
    fi
fi

# ── Generate / build ──────────────────────────────────────────────────────
GENERATED=0
if [ "$DO_GENERATE" -eq 1 ]; then
    echo "== Generating src/gen =="
    # Generate straight from where the ROM already lives; nothing copies it
    # into the repo, and .gitignore blocks it if anyone tries.
    #
    # SNESRECOMP_ROOT is honoured if the caller already set it, so a framework
    # developer can scaffold against a working tree instead of the pinned
    # submodule. Otherwise the submodule is the framework, which is what a
    # normal run wants.
    if SNESRECOMP_ROOT="${SNESRECOMP_ROOT:-snesrecomp}" \
        bash tools/regen.sh --rom "$ROM_ABS"; then
        GENERATED=1
    else
        echo "warning: generation failed — fix recomp/ and re-run tools/regen.sh" >&2
    fi
fi

BUILT=0
GAME_EXE=""
if [ "$DO_BUILD" -eq 1 ] && [ "$GENERATED" -eq 1 ]; then
    echo "== Building =="
    if cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null &&
       cmake --build build -j >/dev/null; then
        BUILT=1
        GAME_EXE="build/$PROJECT_NAME"
        [ -f "$GAME_EXE.exe" ] && GAME_EXE="$GAME_EXE.exe"
    else
        echo "warning: build failed — see the output above." >&2
        echo "         A fresh scaffold is not expected to run yet; src/game_rtl.c" >&2
        echo "         is where the port starts." >&2
    fi
fi

# ── Push ──────────────────────────────────────────────────────────────────
if [ "$GITHUB_CREATED" -eq 1 ]; then
    echo "== Pushing =="
    git push -q -u origin HEAD && echo "  pushed to $GITHUB_OWNER/$GITHUB_REPO"
    if [ "$ENABLE_CI" -eq 1 ]; then
        gh workflow list --repo "$GITHUB_OWNER/$GITHUB_REPO" 2>/dev/null \
            || echo "  (could not list workflows — check Actions in the browser)"
    fi
fi

echo
echo "Ready: $ROOT"
if [ "$GENERATED" -eq 1 ]; then
    echo "  generated: $(find src/gen -name '*.c' | wc -l | tr -d ' ') C files in src/gen"
    echo "  Expected build result: generated-code static library only"
fi
if [ "$BUILT" -eq 1 ] && [ -f "$GAME_EXE" ]; then
    echo "  built:     $GAME_EXE ($(du -h "$GAME_EXE" | cut -f1))"
    echo
    echo "Run it:"
    echo "  '$ROOT/$GAME_EXE' '$ROM_ABS'"
    echo
    echo "It will not play yet — src/game_rtl.c is where the port starts."
fi
echo
echo "Next:"
echo "  cd '$ROOT'"
if [ -n "$FRAMEWORK_GAPS" ]; then
    # Do not hand out a command that will fail the same way it just did.
    echo "  # the pinned framework is missing what this project needs (above);"
    echo "  # fix that first, then:"
fi
[ "$GENERATED" -eq 1 ] || echo "  bash tools/regen.sh --rom '$ROM_ABS'"
[ "$BUILT" -eq 1 ] || echo "  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j"
if [ "$GITHUB_RECOVERY" -eq 1 ]; then
    echo "  gh repo create $GITHUB_OWNER/$GITHUB_REPO --$GITHUB_VISIBILITY --source . --remote=origin"
    echo "  git push -u origin HEAD"
fi
echo "  \$EDITOR src/game_rtl.c       # the port starts here"
