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
#   --rollback                      same as --netplay: every netplay build
#                                   carries the rollback engine (no --no-rollback)
#   --ci / --no-ci                  .github/workflows/release.yml (default: on)
#   --fetch-boxart / --no-fetch-boxart   "Fetch boxart and metadata": libretro
#                                   Named_Boxarts for the launcher, plus publisher,
#                                   developer and year from libretro-database
#                                   (asked on a terminal; off for scripts)
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
# Only a real snesrecomp checkout may supply the framework URL, ref, LICENSE
# and local commits below. This script is also VENDORED into Retro Studio
# (tools/new_project_layout/snes/), where two levels up is Studio's own
# repository -- and deriving the URL from there added Retro-Studio itself as
# the snesrecomp submodule: a scaffold with no runner and no CLI, which
# failed to generate and failed to configure. A vendored copy pins the
# canonical remote.
if [ -f "$FRAMEWORK_ROOT/runner/runner.cmake" ] && [ -f "$FRAMEWORK_ROOT/snesrecomp_cli.py" ]; then
    FRAMEWORK_IS_CHECKOUT=1
else
    FRAMEWORK_IS_CHECKOUT=0
fi
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

# Where custom art goes when none was fetched: the launcher stages
# launcher_assets/img/boxart.tga beside the executable (CMakeLists.txt's
# BOXART argument, EXISTS-guarded, so the file can arrive at any time).
boxart_advice() {
    echo "  No boxart in the project. To add your own, place it at:"
    echo "    launcher_assets/img/boxart.tga   (32-bit uncompressed TGA; the launcher's art)"
    echo "    launcher_assets/img/boxart.png   (optional copy for the README)"
    echo "  It is staged beside the executable on the next build."
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
SET_NETPLAY=0; SET_CI=0; SET_RECOMP_UI=0
SET_GENERATE=0; SET_BUILD=0; SET_GITHUB=0
GITHUB_OWNER="TechnicallyComputers"; GITHUB_REPO=""
ENABLE_NETPLAY=0; ENABLE_CI=1; ENABLE_RECOMP_UI=1
FETCH_BOXART=0; SET_BOXART=0
ADD_SUBMODULES=1
DO_GENERATE=0; DO_BUILD=0; CREATE_GITHUB=0; GITHUB_VISIBILITY="private"
# Default the framework ref to the branch this checkout is on, for the same
# reason the URL is derived: a scaffold cut from a checkout should pin the
# framework that checkout actually has. Hard-coding "main" silently produced
# projects that could not generate or build whenever the work lived on a
# branch.
SNESRECOMP_REF=""
if [ "$FRAMEWORK_IS_CHECKOUT" -eq 1 ]; then
    SNESRECOMP_REF=$(git -C "$FRAMEWORK_ROOT" symbolic-ref --quiet --short HEAD 2>/dev/null || true)
fi
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
SNESRECOMP_URL=""
if [ "$FRAMEWORK_IS_CHECKOUT" -eq 1 ]; then
    SNESRECOMP_URL=$(git -C "$FRAMEWORK_ROOT" remote get-url origin 2>/dev/null || true)
fi
[ -n "$SNESRECOMP_URL" ] || SNESRECOMP_URL="https://github.com/RetroPortingToolKit/snesrecomp.git"
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
        --no-netplay) ENABLE_NETPLAY=0; SET_NETPLAY=1; shift ;;
        --rollback)
            # Rollback is part of netplay now, so the flag means "netplay".
            ENABLE_NETPLAY=1; SET_NETPLAY=1; shift ;;
        --no-rollback)
            echo "note: --no-rollback is ignored -- every netplay build carries the rollback engine" >&2
            shift ;;
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
        --recomp-ui-ref) RECOMP_UI_REF=$2; SET_RECOMP_UI_REF=1; shift 2 ;;
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
ROM_MD5=$(probe_get md5)
ROM_SHA1=$(probe_get sha1)
ROM_SIZE=$(probe_get rom_size)
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

# The title comes from the probe (the cartridge header, then the filename),
# the same way Studio names a project; the folder and CMake name follow from
# it (<Title>SNESRecomp). It is asked only when the probe produced nothing;
# --name overrides it.
if [ -z "$NAME" ]; then
    if [ -n "$SUGGESTED_NAME" ]; then
        NAME="$SUGGESTED_NAME"
        [ "$INTERACTIVE" -eq 0 ] || echo "  title: $NAME (from the ROM; --name to override)"
    elif [ "$INTERACTIVE" -eq 1 ]; then
        echo
        prompt_line "Display name" NAME ""
    fi
fi
[ -n "$NAME" ] || { echo "setup_project: a display name is required (--name)" >&2; exit 1; }

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

# "Fetch boxart and metadata": one question covers the launcher's boxart
# (libretro Named_Boxarts, fetched into the project below) and the README's
# publisher / developer / year (libretro-database, keyed by the ROM's CRC32,
# fetched now so the rendered files carry them). Both need the network, so a
# non-interactive run stays offline unless --fetch-boxart says otherwise.
# What the fetch does not supply -- libretro has no marketing descriptions --
# is asked afterwards, and only then.
if [ "$SET_BOXART" -eq 0 ] && [ "$INTERACTIVE" -eq 1 ]; then
    prompt_yn "Fetch boxart and metadata from libretro? (needs the network)" FETCH_BOXART 1
fi
DEVELOPER=""
if [ "$FETCH_BOXART" -eq 1 ]; then
    echo "== Fetching metadata (libretro-database, crc32 $ROM_CRC32) =="
    META_JSON=$(mktemp)
    if "$PYTHON" "$SCRIPT_DIR/fetch_metadata.py" --crc32 "$ROM_CRC32" \
            --json-out "$META_JSON" >/dev/null 2>&1; then
        meta_get() { "$PYTHON" -c 'import json,sys; print(json.load(open(sys.argv[1])).get(sys.argv[2], ""))' "$META_JSON" "$1"; }
        [ -n "$PUBLISHER" ] || PUBLISHER=$(meta_get publisher)
        [ -n "$YEAR" ] || YEAR=$(meta_get year)
        DEVELOPER=$(meta_get developer)
        echo "  publisher: ${PUBLISHER:--}  developer: ${DEVELOPER:--}  year: ${YEAR:--}"
    else
        echo "  no libretro-database entry for this ROM (crc32 $ROM_CRC32)"
    fi
    rm -f "$META_JSON"
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

if [ "$PLAYERS" -eq 1 ]; then
    if [ "$ENABLE_NETPLAY" -eq 1 ]; then
        echo "warning: netplay ignored for a 1-player title." >&2
    fi
    ENABLE_NETPLAY=0
    SET_NETPLAY=1
elif [ "$SET_NETPLAY" -eq 0 ] && [ "$INTERACTIVE" -eq 1 ]; then
    prompt_yn "Enable netplay (recomp-net)?" ENABLE_NETPLAY 0
fi
# No rollback question: every netplay build carries retcomm-rbengine
# (snesrecomp_enable_recomp_net links it), delay-sync remains the default.

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
if [ "$ENABLE_NETPLAY" -eq 1 ]; then
    PLAYERS_NOTE="$PLAYERS_NOTE

Netplay is built in: delay-sync by default, rollback available
(\`SNES_NET_MODE=rollback\`). See \`snesrecomp/docs/RECOMP_NET.md\` and
\`snesrecomp/docs/ROLLBACK.md\`."
fi

NETPLAY_BLOCK="# Netplay is not built into this target."
if [ "$ENABLE_NETPLAY" -eq 1 ]; then
    NETPLAY_BLOCK="snesrecomp_enable_recomp_net($PROJECT_NAME)"
fi
# Rollback needs no line of its own: snesrecomp_enable_recomp_net links
# retcomm-rbengine for every netplay port.
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
DEVELOPER_DISP=${DEVELOPER:-—}
YEAR_DISP=${YEAR:-—}
DESCRIPTION_MD="$DESCRIPTION"
[ -n "$DESCRIPTION_MD" ] || DESCRIPTION_MD="_Add a short description here._"

echo "== New SNES project =="
echo "  repo:       $ROOT"
echo "  title:      $NAME"
echo "  rom:        $ROM_FILE ($ROM_MAPPING, $REGION, crc32 $ROM_CRC32)"
echo "  zip prefix: $ZIP_PREFIX"
echo "  players:    $PLAYERS (multitap: $MULTITAP)"
echo "  netplay:    $ENABLE_NETPLAY (rollback engine included with netplay)"
if [ "$ENABLE_RECOMP_UI" -eq 1 ]; then
    echo "  recomp-ui:  $RECOMP_UI_REF (boxart + metadata fetched: $FETCH_BOXART)"
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
            if [ "$_what" = "snesrecomp" ] && [ "$FRAMEWORK_IS_CHECKOUT" -eq 1 ] &&
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
        --set "ROM_MD5=$ROM_MD5" \
        --set "ROM_SHA1=$ROM_SHA1" \
        --set "ROM_SIZE=$ROM_SIZE" \
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
        --set "DEVELOPER=$DEVELOPER_DISP" \
        --set "YEAR=$YEAR_DISP" \
        --set "GITHUB_OWNER=$GITHUB_OWNER" \
        --set "GITHUB_REPO=$GITHUB_REPO"
}

# Every templated file, in one place, so it can be rendered twice: once now
# from the copy of the wizard that is running, and again below from the
# framework the project actually pins (see "Rendering from the pinned
# framework").
render_templates() {
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
    if [ "$ENABLE_CI" -eq 1 ]; then
        fill release.yml.in .github/workflows/release.yml
    fi
}
render_templates

# Empty mod catalog. CMakeLists.txt declares it to the framework
# (snesrecomp_target_mod_catalog), which stages packages/ beside the executable
# on every build of it, and the runtime initializes from mods/preloaded there;
# an empty catalog is a valid one (the Mods page just lists nothing).
cat > mods/preloaded/README.md <<EOF
# Preloaded mods

Ship reviewed, default-disabled packages here:

\`\`\`text
packages/<package-id>/<version>/
  manifest.toml
  ...
\`\`\`

A manifest's \`[[target]]\` names this title as \`game_id = "$GAME_ID"\` with the
ROM's SHA-256 (both live in \`rom_identity.txt\`). CMakeLists.txt declares this
directory with \`snesrecomp_target_mod_catalog\` and the FRAMEWORK stages
\`packages/\` beside the executable on every build -- do not add a copy step of
your own, and do not spell the destination: it belongs to snesrecomp, so a
future change to the layout touches one file instead of every port. Nothing
placed beside the executable by hand survives a build.
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
if [ "$FRAMEWORK_IS_CHECKOUT" -eq 1 ]; then
    cp "$FRAMEWORK_ROOT/LICENSE" "$ROOT/LICENSE" 2>/dev/null || true
fi

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
        echo "warning: boxart fetch failed." >&2
        boxart_advice
    fi
elif [ "$ENABLE_RECOMP_UI" -eq 1 ]; then
    boxart_advice
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
# The recomp-ui ref, like the templates below, is the PINNED framework's
# call (its lobby client compiles against recomp-ui's API), not this copy of
# the wizard's -- unless --recomp-ui-ref said otherwise.
if [ -z "${SET_RECOMP_UI_REF:-}" ] && [ -f snesrecomp/tools/new_project/RECOMP_UI_REF ]; then
    _pinned_ui_ref=$(sed -n '1{s/[[:space:]]*$//;p}' snesrecomp/tools/new_project/RECOMP_UI_REF)
    if [ -n "$_pinned_ui_ref" ] && [ "$_pinned_ui_ref" != "$RECOMP_UI_REF" ]; then
        echo "== recomp-ui ref from the pinned framework: $_pinned_ui_ref (this wizard said $RECOMP_UI_REF) =="
        RECOMP_UI_REF=$_pinned_ui_ref
    fi
fi
if [ "$ENABLE_RECOMP_UI" -eq 1 ]; then
    git submodule add -q -b "$RECOMP_UI_REF" "$RECOMP_UI_URL" recomp-ui
fi
git submodule update --init --recursive

# What was just pinned must BE the framework. The gap check further down
# compares subcommands; a repository that is not snesrecomp at all (the URL
# was derived from the wrong checkout, or mistyped) has no CLI to compare
# and used to be reported as "the framework does not support generate yet
# -- the scaffold is still correct", which it was not.
if [ ! -f snesrecomp/runner/runner.cmake ] || [ ! -f snesrecomp/snesrecomp_cli.py ]; then
    echo "setup_project: $SNESRECOMP_URL ($SNESRECOMP_REF) is not a snesrecomp" >&2
    echo "  checkout: it has no runner/runner.cmake or snesrecomp_cli.py. Pass" >&2
    echo "  --snesrecomp-url <url> (and --snesrecomp-ref <ref>) naming the" >&2
    echo "  framework repository. Nothing has been kept." >&2
    exit 1
fi

# Render from the framework the project PINS, not from the copy of this wizard
# that happens to be running. They are the same files only when this script
# runs out of the checkout that becomes the submodule; Studio runs it from a
# sibling checkout or its vendored copy, and a stale one there rendered a
# host that predated the framework it pinned -- a scaffold that built, then
# "did not boot", with nothing in it saying why. The pinned framework's
# templates and its fill_tokens.py are authoritative for the code they
# generate; a newer framework that needs a token this wizard does not know
# fails loudly here rather than producing a project that is quietly wrong.
if [ -f "$ROOT/snesrecomp/tools/new_project/templates/main.c.in" ]; then
    _pinned_wizard="$ROOT/snesrecomp/tools/new_project"
    if [ "$(cd "$_pinned_wizard" && pwd)" != "$SCRIPT_DIR" ]; then
        echo "== Rendering from the pinned framework ($(git -C snesrecomp rev-parse --short HEAD)) =="
        if ! diff -rq "$TEMPLATE_DIR" "$_pinned_wizard/templates" >/dev/null 2>&1; then
            echo "   (this wizard's own templates differ from the pinned framework's;"
            echo "    the pinned ones win -- they match the runtime the project builds)"
        fi
        TEMPLATE_DIR="$_pinned_wizard/templates"
        FILL_TOKENS="$_pinned_wizard/fill_tokens.py"
        if ! render_templates; then
            echo "setup_project: the pinned framework's templates could not be" >&2
            echo "  rendered by this copy of the wizard (it is older than the" >&2
            echo "  framework). Run snesrecomp/tools/new_project/setup_project.sh" >&2
            echo "  from a checkout of the ref you are pinning instead." >&2
            exit 1
        fi
    fi
fi

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
# Every netplay build links retcomm-rbengine, so its absence is a gap.
if [ "$ENABLE_NETPLAY" -eq 1 ] && [ ! -f snesrecomp/lib/retcomm-rbengine/CMakeLists.txt ]; then
    note_gap "rollback engine: snesrecomp/lib/retcomm-rbengine is missing (git submodule update --init --recursive)"
elif ! grep -q "snesrecomp_enable_rollback" snesrecomp/runner/recomp_net.cmake 2>/dev/null; then
    note_gap "rollback engine: the framework has no snesrecomp_enable_rollback()"
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
        echo "         A fresh scaffold is expected to build; the framework host" >&2
        echo "         and the template frame model are complete. The failure" >&2
        echo "         above is a toolchain or dependency problem, not the port." >&2
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
fi
if [ "$BUILT" -eq 1 ] && [ -f "$GAME_EXE" ]; then
    echo "  built:     $GAME_EXE ($(du -h "$GAME_EXE" | cut -f1))"
    echo
    echo "Run it:"
    echo "  '$ROOT/$GAME_EXE' '$ROM_ABS'"
    echo
    echo "Boots to a black screen? src/game_rtl.c (the frame model) is where the port starts."
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
