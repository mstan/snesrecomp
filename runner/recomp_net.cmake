# recomp_net.cmake — optional recomp-net delay-sync netcode for snesrecomp games.
#
# recomp-net lives as a git submodule at lib/recomp-net. Games that want
# multiplayer link the STATIC target `recomp_net` (alias `recomp_net::recomp_net`)
# and implement the host loop described in lib/recomp-net/docs/host_integration.md
# (and docs/RECOMP_NET.md in this repo).
#
# Usage from a game project's CMakeLists.txt (after include(runner.cmake)):
#
#   # Option A — helper (preferred): add_subdirectory once + link the target
#   snesrecomp_enable_recomp_net(MyGame)
#
#   # Option B — opt-in via cache before include(runner.cmake):
#   #   cmake -DSNESRECOMP_ENABLE_NET=ON ...
#   # then link ${SNESRECOMP_RUNNER_LIBRARIES} (includes recomp_net when enabled)
#
# ICE / WAN (libjuice) — set before enabling:
#   set(SNESRECOMP_NET_ICE ON CACHE BOOL "" FORCE)
#   # or: cmake -DSNESRECOMP_NET_ICE=ON ...

if(NOT SNESRECOMP_RECOMP_NET_ROOT)
    get_filename_component(SNESRECOMP_RECOMP_NET_ROOT
        "${CMAKE_CURRENT_LIST_DIR}/../lib/recomp-net" ABSOLUTE)
endif()

option(SNESRECOMP_ENABLE_NET
    "Build and expose recomp-net (delay-sync netcode) for game targets" OFF)
option(SNESRECOMP_NET_ICE
    "Enable ICE/libjuice transport in recomp-net (needs network at configure if libjuice is not vendored)" OFF)

# Internal: add_subdirectory once; disable examples/tests when embedded.
function(_snesrecomp_add_recomp_net)
    if(TARGET recomp_net)
        return()
    endif()

    if(NOT EXISTS "${SNESRECOMP_RECOMP_NET_ROOT}/CMakeLists.txt")
        message(FATAL_ERROR
            "recomp-net submodule missing at ${SNESRECOMP_RECOMP_NET_ROOT}.\n"
            "Run: git submodule update --init --recursive lib/recomp-net")
    endif()

    set(RNET_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(RNET_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    if(SNESRECOMP_NET_ICE)
        set(RNET_ENABLE_ICE ON CACHE BOOL "" FORCE)
    else()
        set(RNET_ENABLE_ICE OFF CACHE BOOL "" FORCE)
    endif()

    add_subdirectory(
        "${SNESRECOMP_RECOMP_NET_ROOT}"
        "${CMAKE_BINARY_DIR}/recomp-net-build"
        EXCLUDE_FROM_ALL)

    if(NOT TARGET recomp_net)
        message(FATAL_ERROR "recomp-net CMake did not create target recomp_net")
    endif()
endfunction()

# Link recomp-net into a game executable (and ensure it is built).
function(snesrecomp_enable_recomp_net target)
    if(NOT TARGET ${target})
        message(FATAL_ERROR
            "snesrecomp_enable_recomp_net: '${target}' is not a CMake target. "
            "Call this after add_executable(${target} ...).")
    endif()
    _snesrecomp_add_recomp_net()
    target_link_libraries(${target} PRIVATE recomp_net)
    target_compile_definitions(${target} PRIVATE SNESRECOMP_NET=1)
    # SNES host facade and launcher-facing lobby client. The lobby remains
    # transport/UI agnostic; recomp-ui consumes it through game callbacks.
    if(NOT SNESRECOMP_ENABLE_NET)
        target_sources(${target} PRIVATE
            "${SNESRECOMP_RUNNER_ROOT}/src/netplay/snes_netplay.c"
            "${SNESRECOMP_RUNNER_ROOT}/src/lobby/snes_lobby_client.c"
            "${SNESRECOMP_RUNNER_ROOT}/src/lobby/ws/rnet_ws.c"
            "${SNESRECOMP_RUNNER_ROOT}/src/lobby/ws/rnet_sha1.c")
        target_include_directories(${target} PRIVATE
            "${SNESRECOMP_RUNNER_ROOT}/src/netplay"
            "${SNESRECOMP_RUNNER_ROOT}/src/lobby"
            "${SNESRECOMP_RUNNER_ROOT}/src/lobby/ws")
    endif()
    target_compile_definitions(${target} PRIVATE SNES_HAS_LOBBY_CLIENT=1)
    if(WIN32)
        target_link_libraries(${target} PRIVATE ws2_32)
    endif()
endfunction()

# When SNESRECOMP_ENABLE_NET=ON at configure time, pull the library into the
# shared runner link list so games that already link ${SNESRECOMP_RUNNER_LIBRARIES}
# pick it up without a separate helper call.
if(SNESRECOMP_ENABLE_NET)
    _snesrecomp_add_recomp_net()
    list(APPEND SNESRECOMP_RUNNER_SOURCES
        "${SNESRECOMP_RUNNER_ROOT}/src/netplay/snes_netplay.c"
        "${SNESRECOMP_RUNNER_ROOT}/src/lobby/snes_lobby_client.c"
        "${SNESRECOMP_RUNNER_ROOT}/src/lobby/ws/rnet_ws.c"
        "${SNESRECOMP_RUNNER_ROOT}/src/lobby/ws/rnet_sha1.c")
    list(APPEND SNESRECOMP_RUNNER_LIBRARIES recomp_net)
    list(APPEND SNESRECOMP_RUNNER_INCLUDE_DIRS
        "${SNESRECOMP_RECOMP_NET_ROOT}/include"
        "${SNESRECOMP_RUNNER_ROOT}/src/netplay"
        "${SNESRECOMP_RUNNER_ROOT}/src/lobby"
        "${SNESRECOMP_RUNNER_ROOT}/src/lobby/ws")
    add_compile_definitions(SNESRECOMP_NET=1 SNES_HAS_LOBBY_CLIENT=1)
    if(WIN32)
        list(APPEND SNESRECOMP_RUNNER_LIBRARIES ws2_32)
    endif()
    message(STATUS
        "SNESRECOMP_ENABLE_NET: recomp-net linked via SNESRECOMP_RUNNER_LIBRARIES"
        " (ICE=${SNESRECOMP_NET_ICE})")
endif()
