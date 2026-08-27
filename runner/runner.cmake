# runner.cmake — shared source list for snesrecomp game projects.
#
# Usage from a game project's CMakeLists.txt:
#   set(SNESRECOMP_ROOT ${CMAKE_SOURCE_DIR}/snesrecomp)
#   include(${SNESRECOMP_ROOT}/runner/runner.cmake)
#   add_executable(MyGame ${SNESRECOMP_RUNNER_SOURCES} <game sources> <generated sources>)
#   target_include_directories(MyGame PRIVATE ${SNESRECOMP_RUNNER_INCLUDE_DIRS} ...)
#
# Mirrors the file list in the MSVC .vcxproj so the same sources build on
# Windows (MSVC) and on macOS/Linux (clang/gcc + CMake). The snes9x emulator
# oracle (snes9x_bridge.cpp / ENABLE_ORACLE_BACKEND) is intentionally NOT part
# of this list — it is a developer-only verify backend, off for normal builds.

set(SNESRECOMP_RUNNER_ROOT ${CMAKE_CURRENT_LIST_DIR})

# Desktop SDL selection is shared by every game host. SDL3 is the default;
# SDL2 remains available as an explicit compatibility fallback:
#
#   cmake -S . -B build                         # SDL3
#   cmake -S . -B build-sdl2 -DSNESRECOMP_SDL_BACKEND=SDL2
#
# Keep discovery and target wiring in one helper so games do not grow their
# own subtly different SDL package/link rules.
set(SNESRECOMP_SDL_BACKEND "SDL3" CACHE STRING
    "Desktop SDL backend (SDL3 or SDL2)")
set_property(CACHE SNESRECOMP_SDL_BACKEND PROPERTY STRINGS SDL3 SDL2)
string(TOUPPER "${SNESRECOMP_SDL_BACKEND}" _SNESRECOMP_SDL_BACKEND)
if(NOT _SNESRECOMP_SDL_BACKEND STREQUAL "SDL3" AND
   NOT _SNESRECOMP_SDL_BACKEND STREQUAL "SDL2")
    message(FATAL_ERROR
        "SNESRECOMP_SDL_BACKEND must be SDL3 or SDL2 "
        "(got '${SNESRECOMP_SDL_BACKEND}')")
endif()
set(SNESRECOMP_SDL_BACKEND "${_SNESRECOMP_SDL_BACKEND}" CACHE STRING
    "Desktop SDL backend (SDL3 or SDL2)" FORCE)

function(snesrecomp_target_sdl target)
    if(_SNESRECOMP_SDL_BACKEND STREQUAL "SDL3")
        find_package(SDL3 CONFIG REQUIRED)
        target_link_libraries(${target} PRIVATE SDL3::SDL3)
        target_compile_definitions(${target} PRIVATE
            SNESRECOMP_SDL3=1
            LNG_SDL3=1
            SDL_MAIN_HANDLED)
        if(WIN32 AND TARGET SDL3::SDL3-shared)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:SDL3::SDL3-shared>
                    $<TARGET_FILE_DIR:${target}>)
        endif()
        message(STATUS "${target}: SDL3 desktop backend")
    else()
        find_package(SDL2 REQUIRED)
        target_compile_definitions(${target} PRIVATE
            SNESRECOMP_SDL3=0
            SDL_MAIN_HANDLED)
        if(TARGET SDL2::SDL2)
            target_link_libraries(${target} PRIVATE SDL2::SDL2)
            if(WIN32)
                get_target_property(_snesrecomp_sdl2_type SDL2::SDL2 TYPE)
                if(_snesrecomp_sdl2_type STREQUAL "SHARED_LIBRARY")
                    add_custom_command(TARGET ${target} POST_BUILD
                        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            $<TARGET_FILE:SDL2::SDL2>
                            $<TARGET_FILE_DIR:${target}>)
                endif()
            endif()
        else()
            target_include_directories(${target} PRIVATE ${SDL2_INCLUDE_DIRS})
            set(_snesrecomp_sdl2_libraries ${SDL2_LIBRARIES})
            if(WIN32)
                list(FILTER _snesrecomp_sdl2_libraries
                    EXCLUDE REGEX "SDL2main")
            endif()
            target_link_libraries(${target} PRIVATE
                ${_snesrecomp_sdl2_libraries})
        endif()
        message(STATUS "${target}: SDL2 compatibility backend")
    endif()
endfunction()

set(SNESRECOMP_RUNNER_SOURCES
    ${SNESRECOMP_RUNNER_ROOT}/src/common_cpu_infra.c
    ${SNESRECOMP_RUNNER_ROOT}/src/common_rtl.c
    ${SNESRECOMP_RUNNER_ROOT}/src/widescreen.c
    ${SNESRECOMP_RUNNER_ROOT}/src/recomp_hw.c
    ${SNESRECOMP_RUNNER_ROOT}/src/framedump.c
    ${SNESRECOMP_RUNNER_ROOT}/src/host_paths.c
    ${SNESRECOMP_RUNNER_ROOT}/src/launcher.c
    ${SNESRECOMP_RUNNER_ROOT}/src/launcher_cache.c
    ${SNESRECOMP_RUNNER_ROOT}/src/launcher_picker.c
    ${SNESRECOMP_RUNNER_ROOT}/src/rom_image_verify.c
    ${SNESRECOMP_RUNNER_ROOT}/src/crc32.c
    ${SNESRECOMP_RUNNER_ROOT}/src/sha256.c
    ${SNESRECOMP_RUNNER_ROOT}/src/keybinds.c
    ${SNESRECOMP_RUNNER_ROOT}/src/cpu_state.c
    ${SNESRECOMP_RUNNER_ROOT}/src/cpu_trace.c
    ${SNESRECOMP_RUNNER_ROOT}/src/audio_trace.c
    ${SNESRECOMP_RUNNER_ROOT}/src/ppu_dma_trace.c
    ${SNESRECOMP_RUNNER_ROOT}/src/host_report.c
    ${SNESRECOMP_RUNNER_ROOT}/src/execution_mode.c
    ${SNESRECOMP_RUNNER_ROOT}/src/util.c
    # SNES hardware model
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/apu.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/cart.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/cpu.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/dma.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/dsp.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/dsp1.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/dsp1_hle.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/joypad.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/audio_shadow.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/dsp_shadow.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/msu1.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/color_lut.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/ppu.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/ppu_legacy.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/sa1.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/ws_shadow.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/snes.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/snes_other.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/spc.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/superfx.c
    # Interpreter-fallback tier (docs/MULTI_TIER.md): LakeSnes core + bridge.
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/interp816.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/tier2_capture.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/interp_bridge.c
)

# ── Capcom Cx4 coprocessor (Mega Man X2 / X3 only) ────────────────────────
# cx4.c is an instruction-level Hitachi HG51B S169 core ported from ares
# (ISC licence — permissive, notice-only). It is the faithful LLE floor; any
# future host-side Cx4 shortcut must be a gated optimization layered ON TOP of
# it, authored from this core's observed behavior.
#
list(APPEND SNESRECOMP_RUNNER_SOURCES
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/cx4.c)
message(STATUS "Cx4: instruction-level HG51B S169 core (ares, ISC)")

# The TCP debug server + emulator-oracle command handlers are a developer-only
# feature. debug_server.h provides static-inline no-op stubs when SNESRECOMP_TRACE
# is 0 (the default), so debug_server.c must only be compiled when tracing is on —
# otherwise the real definitions collide with the header stubs. Off by default for
# a normal playable build; opt in with -DSNESRECOMP_ENABLE_TRACE=ON.
option(SNESRECOMP_ENABLE_TRACE "Build the TCP debug server / observability rings" OFF)
if(SNESRECOMP_ENABLE_TRACE)
    # Compiling debug_server.c is only half of turning tracing on. The header
    # keys off SNESRECOMP_TRACE, not off the option, and defaults it to 0 — so
    # without this define every translation unit (debug_server.c included) sees
    # the no-op stubs, and debug_server.c's real definitions collide with the
    # stubs it just pulled in from its own header. The option was inert before
    # this: -DSNESRECOMP_ENABLE_TRACE=ON did not build, it only failed.
    add_compile_definitions(SNESRECOMP_TRACE=1)
    list(APPEND SNESRECOMP_RUNNER_SOURCES
        ${SNESRECOMP_RUNNER_ROOT}/src/debug_server.c
        # debug_server.c's on-demand dump calls recomp_post_mortem_dump(), and
        # this is the only translation unit that defines it. It is not in the
        # base source list, so a trace build does not link without it.
        ${SNESRECOMP_RUNNER_ROOT}/src/desktop/post_mortem.c
    )
    if(EXISTS ${SNESRECOMP_RUNNER_ROOT}/src/emu_oracle_cmds.c)
        list(APPEND SNESRECOMP_RUNNER_SOURCES
            ${SNESRECOMP_RUNNER_ROOT}/src/emu_oracle_cmds.c
        )
    endif()
endif()

# Schema-driven mod packages and trusted static plugins. This is deliberately
# opt-in: ordinary games do not compile the loader, expose a Mods navigation
# item, create a mods directory, or change any runtime behavior. A game that
# opts in owns its recomp-ui pin, package catalog, and statically linked plugin
# implementations.
option(SNESRECOMP_ENABLE_MODS
    "Build the SNES mod package loader and trusted-plugin runtime"
    OFF)
if(SNESRECOMP_ENABLE_MODS)
    list(APPEND SNESRECOMP_RUNNER_SOURCES
        ${SNESRECOMP_RUNNER_ROOT}/src/mod_runtime.cpp
    )
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    add_compile_definitions(SNESRECOMP_ENABLE_MODS=1)
    # recomp-ui still requires a non-null provider at runtime. This enables
    # only the UI half of that double gate for the explicitly opting-in game.
    set(RECOMP_UI_ENABLE_MODS ON CACHE BOOL
        "Enable recomp-ui Mods view for this SNES mod-enabled game" FORCE)
    message(STATUS
        "SNES mods: package loader + trusted static plugins enabled")
else()
    add_compile_definitions(SNESRECOMP_ENABLE_MODS=0)
endif()

set(SNESRECOMP_RUNNER_LIBRARIES)
if(NOT WIN32)
    # cx4.c synthesizes its internal data ROM with libm.
    list(APPEND SNESRECOMP_RUNNER_LIBRARIES m)
endif()
if(SNESRECOMP_ENABLE_TRACE AND WIN32)
    list(APPEND SNESRECOMP_RUNNER_LIBRARIES ws2_32)
endif()

# Differential co-simulation (SNES_COSIM.md): full-state first-divergence oracle.
# DEV/DIAGNOSTICS ONLY — must NEVER be enabled in a shipping Production config.
# Adds the frame-keyed park/step engine (cosim.c) + canonical state hash
# (cosim_state.c) + a loopback TCP server; needs ws2_32 on Windows. Defines
# SNES_COSIM for every target configured after this include (the game exe).
option(SNES_COSIM "Build the differential co-simulation engine (DEV ONLY)" OFF)
if(SNES_COSIM)
    list(APPEND SNESRECOMP_RUNNER_SOURCES
        ${SNESRECOMP_RUNNER_ROOT}/src/cosim.c
        ${SNESRECOMP_RUNNER_ROOT}/src/cosim_state.c
    )
    add_compile_definitions(SNES_COSIM)
    if(WIN32)
        list(APPEND SNESRECOMP_RUNNER_LIBRARIES ws2_32)
    endif()
    message(STATUS "SNES_COSIM enabled — DEV co-simulation build (not for release)")
endif()

# Full-WRAM frame fingerprints are a forensic determinism aid, not emulated
# hardware. A trace build keeps the historical behavior by default, while a
# normal production build avoids hashing all 128 KiB of WRAM every frame and
# does not reserve the ring. Co-simulation always requires the fingerprints,
# regardless of an explicitly disabled cache option.
option(SNESRECOMP_ENABLE_FRAME_FINGERPRINTS
    "Hash full WRAM each frame for diagnostic determinism history"
    ${SNESRECOMP_ENABLE_TRACE})
if(SNES_COSIM OR SNESRECOMP_ENABLE_FRAME_FINGERPRINTS)
    set(_SNESRECOMP_FRAME_FINGERPRINTS 1)
    message(STATUS "SNES frame fingerprints: enabled")
else()
    set(_SNESRECOMP_FRAME_FINGERPRINTS 0)
    message(STATUS "SNES frame fingerprints: disabled (production)")
endif()
set_property(SOURCE
    ${SNESRECOMP_RUNNER_ROOT}/src/common_rtl.c
    ${SNESRECOMP_RUNNER_ROOT}/src/debug_server.c
    APPEND PROPERTY COMPILE_DEFINITIONS
    SNESRECOMP_FRAME_FINGERPRINTS=${_SNESRECOMP_FRAME_FINGERPRINTS})
unset(_SNESRECOMP_FRAME_FINGERPRINTS)

# PPU/DMA history is another forensic facility rather than emulated hardware.
# Its per-frame snapshot counts non-zero entries across all CGRAM and VRAM, so
# trace-off production must not pay that scan or reserve the event rings.
option(SNESRECOMP_ENABLE_PPU_DMA_HISTORY
    "Capture diagnostic PPU snapshots and DMA event history"
    ${SNESRECOMP_ENABLE_TRACE})
if(SNES_COSIM OR SNESRECOMP_ENABLE_PPU_DMA_HISTORY)
    set(_SNESRECOMP_PPU_DMA_HISTORY 1)
    message(STATUS "SNES PPU/DMA history: enabled")
else()
    set(_SNESRECOMP_PPU_DMA_HISTORY 0)
    message(STATUS "SNES PPU/DMA history: disabled (production)")
endif()
set_property(SOURCE
    ${SNESRECOMP_RUNNER_ROOT}/src/ppu_dma_trace.c
    APPEND PROPERTY COMPILE_DEFINITIONS
    SNESRECOMP_PPU_DMA_HISTORY=${_SNESRECOMP_PPU_DMA_HISTORY})
unset(_SNESRECOMP_PPU_DMA_HISTORY)

# Runtime dispatch hit/miss totals are cheap production health counters. The
# individual event ring is forensic history and writes several fields on every
# indirect AOT/interpreter dispatch, so keep it only in diagnostic builds.
option(SNESRECOMP_ENABLE_DISPATCH_HISTORY
    "Capture diagnostic indirect-dispatch event history"
    ${SNESRECOMP_ENABLE_TRACE})
if(SNES_COSIM OR SNESRECOMP_ENABLE_DISPATCH_HISTORY)
    set(_SNESRECOMP_DISPATCH_HISTORY 1)
    message(STATUS "SNES dispatch history: enabled")
else()
    set(_SNESRECOMP_DISPATCH_HISTORY 0)
    message(STATUS "SNES dispatch history: disabled (production)")
endif()
set_property(SOURCE
    ${SNESRECOMP_RUNNER_ROOT}/src/cpu_state.c
    APPEND PROPERTY COMPILE_DEFINITIONS
    SNESRECOMP_DISPATCH_HISTORY=${_SNESRECOMP_DISPATCH_HISTORY})
unset(_SNESRECOMP_DISPATCH_HISTORY)

set(SNESRECOMP_RUNNER_INCLUDE_DIRS
    ${SNESRECOMP_RUNNER_ROOT}/src
    ${SNESRECOMP_RUNNER_ROOT}/src/snes
)

# Optional desktop GLSL preset renderer. It deliberately stays out of
# SNESRECOMP_RUNNER_SOURCES because headless tools and non-OpenGL frontends do
# not carry the game-owned gl_core/stb/config dependencies it consumes.
function(snesrecomp_target_glsl_shader target)
    target_sources(${target} PRIVATE
        ${SNESRECOMP_RUNNER_ROOT}/src/desktop/glsl_shader.c)
    target_include_directories(${target} PRIVATE
        ${SNESRECOMP_RUNNER_ROOT}/src/desktop)
    if(NOT MSVC)
        target_link_libraries(${target} PRIVATE m)
    endif()
endfunction()

# Shared configuration/keybinding implementation used by the Mega Man X
# trilogy hosts. Kept opt-in because its Config structure and INI grammar are
# intentionally game-facing rather than part of the core runner ABI.
function(snesrecomp_target_mmx_config target)
    target_sources(${target} PRIVATE
        ${SNESRECOMP_RUNNER_ROOT}/src/desktop/mmx_config.c)
    target_include_directories(${target} PRIVATE
        ${SNESRECOMP_RUNNER_ROOT}/src/desktop)
endfunction()

# Shared crash/exit report serializer. Games with interpreter fallback coverage
# can opt into the additional standalone Tier-2 manifest.
function(snesrecomp_target_post_mortem target)
    set(options TIER2)
    cmake_parse_arguments(PM "${options}" "" "" ${ARGN})
    target_sources(${target} PRIVATE
        ${SNESRECOMP_RUNNER_ROOT}/src/desktop/post_mortem.c)
    target_include_directories(${target} PRIVATE
        ${SNESRECOMP_RUNNER_ROOT}/src/desktop)
    if(PM_TIER2)
        target_compile_definitions(${target} PRIVATE
            SNESRECOMP_POST_MORTEM_TIER2=1)
    endif()
    if(WIN32)
        target_link_libraries(${target} PRIVATE dbghelp)
    endif()
endfunction()

# Win32 Fiber API compatibility for non-Windows cooperative schedulers.
function(snesrecomp_target_fiber_compat target)
    target_sources(${target} PRIVATE
        ${SNESRECOMP_RUNNER_ROOT}/src/desktop/fiber_compat.c)
    target_include_directories(${target} PRIVATE
        ${SNESRECOMP_RUNNER_ROOT}/src/desktop)
endfunction()

# Optional delay-sync netcode (lib/recomp-net submodule). See docs/RECOMP_NET.md.
# Does nothing unless SNESRECOMP_ENABLE_NET=ON or the game calls
# snesrecomp_enable_recomp_net(<target>).
include(${SNESRECOMP_RUNNER_ROOT}/recomp_net.cmake)

# Dear ImGui pre-boot launcher is NOT vendored here. Games that need it add
# mstan/recomp-ui as a repo-root submodule and call recomp_target_launcher_ui()
# themselves (see docs/LAUNCHER_DESIGN.md).
