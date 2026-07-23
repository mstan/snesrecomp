# lib/CMakeLists.txt — optional Windows-oriented superbuild helpers.
#
# The Dear ImGui launcher lives in lib/recomp-ui and is wired by
# runner/recomp_ui.cmake (snesrecomp_enable_launcher_ui). Games using CMake
# should prefer that path.
#
# This file optionally builds recomp-net for MSBuild-era consumers that still
# want a packaged static library from lib/:
#   cmake -S lib -B lib/_build -DSNESRECOMP_BUILD_RECOMP_NET=ON
# Prefer runner/recomp_net.cmake (snesrecomp_enable_recomp_net) for new games.

cmake_minimum_required(VERSION 3.20)
project(snesrecomp_lib_helpers CXX C)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

option(SNESRECOMP_BUILD_RECOMP_NET
    "Build the recomp_net static library from this superbuild" OFF)
option(SNESRECOMP_NET_ICE
    "Enable ICE/libjuice when building recomp-net from lib/" OFF)
if(SNESRECOMP_BUILD_RECOMP_NET)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/recomp-net/CMakeLists.txt")
        message(FATAL_ERROR
            "lib/recomp-net missing. Run: git submodule update --init lib/recomp-net")
    endif()
    set(RNET_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(RNET_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    if(SNESRECOMP_NET_ICE)
        set(RNET_ENABLE_ICE ON CACHE BOOL "" FORCE)
    else()
        set(RNET_ENABLE_ICE OFF CACHE BOOL "" FORCE)
    endif()
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/recomp-net"
                     "${CMAKE_BINARY_DIR}/recomp-net-build" EXCLUDE_FROM_ALL)
endif()
