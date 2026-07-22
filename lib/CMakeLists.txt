# lib/CMakeLists.txt — launcher dependency superbuild.
#
# Builds RmlUi (core) + FreeType as STATIC libraries so the hand-maintained
# MSBuild game projects (e.g. SuperMarioWorldRecomp/src/smw.vcxproj) can link
# them without each project re-deriving the dependency graph. The CMake game
# builds (Linux/AppImage) instead use runner/launcher.cmake's
# snesrecomp_ensure_launcher_libs(), which add_subdirectory()'s the very same
# submodules with identical options — this file just packages the Windows path.
#
# Config mirrors psxrecomp/runtime/runtime.cmake exactly (the known-good RmlUi
# 6.0 + FreeType 2.13.3 pin): core font rasteriser only, no optional codecs,
# std:: containers, no samples/tests/PCH.
#
# Build (from snesrecomp/): tools\build_launcher_deps.ps1
#   -> lib/_build/<Config>/{rmlui_core,freetype}.lib

cmake_minimum_required(VERSION 3.20)
project(snesrecomp_launcher_deps CXX C)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Static libs fold into the game exe (self-contained binary).
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# --- FreeType: core rasteriser only, every optional codec/dep disabled. ---
set(FT_DISABLE_ZLIB     TRUE CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2    TRUE CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG      TRUE CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ TRUE CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI   TRUE CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/freetype"
                 "${CMAKE_BINARY_DIR}/freetype-build" EXCLUDE_FROM_ALL)
if(NOT TARGET Freetype::Freetype)
    add_library(Freetype::Freetype ALIAS freetype)
endif()

# --- RmlUi: core only, FreeType font engine, no samples/tests/PCH. ---
set(RMLUI_SAMPLES             OFF        CACHE BOOL   "" FORCE)
set(RMLUI_FONT_ENGINE         "freetype" CACHE STRING "" FORCE)
set(RMLUI_PRECOMPILED_HEADERS OFF        CACHE BOOL   "" FORCE)
set(RMLUI_THIRDPARTY_CONTAINERS OFF      CACHE BOOL   "" FORCE)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/RmlUi"
                 "${CMAKE_BINARY_DIR}/RmlUi-build" EXCLUDE_FROM_ALL)

# --- recomp-net: optional delay-sync netcode (lib/recomp-net submodule). ---
# Off by default so the existing launcher_deps Windows path stays unchanged.
# Enable with: cmake -DSNESRECOMP_BUILD_RECOMP_NET=ON ...
# Games using CMake should prefer runner/recomp_net.cmake
# (snesrecomp_enable_recomp_net / SNESRECOMP_ENABLE_NET) instead.
option(SNESRECOMP_BUILD_RECOMP_NET
    "Also build the recomp_net static library from this superbuild" OFF)
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

# A convenience aggregate so `cmake --build . --target launcher_deps` builds both.
add_custom_target(launcher_deps ALL DEPENDS rmlui_core freetype)
if(SNESRECOMP_BUILD_RECOMP_NET AND TARGET recomp_net)
    add_dependencies(launcher_deps recomp_net)
endif()
