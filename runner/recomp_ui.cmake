# recomp_ui.cmake — optional Dear ImGui pre-boot launcher for snesrecomp games.
#
# recomp-ui lives as a git submodule at lib/recomp-ui. Games that want the
# shared launcher call snesrecomp_enable_launcher_ui(<target>) after
# add_executable(...). That forwards to recomp_ui.cmake's
# recomp_target_launcher_ui() and defines RECOMP_LAUNCHER on the target.
#
# Usage from a game project's CMakeLists.txt (after include(runner.cmake)):
#
#   add_executable(MyGame ...)
#   snesrecomp_enable_launcher_ui(MyGame)
#   # optional art overrides:
#   # snesrecomp_enable_launcher_ui(MyGame
#   #     BOXART ${CMAKE_SOURCE_DIR}/art/boxart.tga
#   #     PAD    ${CMAKE_SOURCE_DIR}/art/pad.tga
#   #     BRAND  ${CMAKE_SOURCE_DIR}/art/brand.tga)
#
# Host main() should call recomp_launcher_run_window() behind #ifdef RECOMP_LAUNCHER
# (see MetalWarriorsSNESRecomp/src/main.c).

if(NOT SNESRECOMP_RECOMP_UI_ROOT)
    get_filename_component(SNESRECOMP_RECOMP_UI_ROOT
        "${CMAKE_CURRENT_LIST_DIR}/../lib/recomp-ui" ABSOLUTE)
endif()

# Allow games/CI to override the checkout (e.g. a worktree of recomp-ui).
# Ignore stale CACHE entries that no longer contain recomp_ui.cmake (common
# after migrating from a game-root submodule to snesrecomp/lib/recomp-ui).
if(DEFINED RECOMP_UI_ROOT AND NOT RECOMP_UI_ROOT STREQUAL "")
    if(EXISTS "${RECOMP_UI_ROOT}/recomp_ui.cmake")
        set(SNESRECOMP_RECOMP_UI_ROOT "${RECOMP_UI_ROOT}")
    else()
        message(STATUS
            "Ignoring RECOMP_UI_ROOT=${RECOMP_UI_ROOT} "
            "(no recomp_ui.cmake); using ${SNESRECOMP_RECOMP_UI_ROOT}")
        unset(RECOMP_UI_ROOT CACHE)
    endif()
endif()

option(SNESRECOMP_LAUNCHER_UI
    "Wire recomp-ui into game targets via snesrecomp_enable_launcher_ui" ON)

function(snesrecomp_enable_launcher_ui target)
    if(NOT SNESRECOMP_LAUNCHER_UI)
        message(STATUS
            "snesrecomp_enable_launcher_ui(${target}): skipped "
            "(SNESRECOMP_LAUNCHER_UI=OFF)")
        return()
    endif()
    if(NOT TARGET ${target})
        message(FATAL_ERROR
            "snesrecomp_enable_launcher_ui: '${target}' is not a CMake target. "
            "Call this after add_executable(${target} ...).")
    endif()
    if(NOT EXISTS "${SNESRECOMP_RECOMP_UI_ROOT}/recomp_ui.cmake")
        message(FATAL_ERROR
            "recomp-ui submodule missing at ${SNESRECOMP_RECOMP_UI_ROOT}.\n"
            "Run: git submodule update --init --recursive lib/recomp-ui")
    endif()

    # recomp_ui.cmake keys off RECOMP_UI_ROOT; point it at our submodule.
    set(RECOMP_UI_ROOT "${SNESRECOMP_RECOMP_UI_ROOT}" CACHE PATH
        "Root directory of the recomp-ui launcher repo" FORCE)
    include(${SNESRECOMP_RECOMP_UI_ROOT}/recomp_ui.cmake)
    # Forward optional BOXART / PAD / BRAND / … args unchanged.
    recomp_target_launcher_ui(${target} ${ARGN})
    message(STATUS
        "snesrecomp launcher UI enabled (${target}) via ${SNESRECOMP_RECOMP_UI_ROOT}")
endfunction()
