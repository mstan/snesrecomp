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

set(SNESRECOMP_RUNNER_SOURCES
    ${SNESRECOMP_RUNNER_ROOT}/src/common_cpu_infra.c
    ${SNESRECOMP_RUNNER_ROOT}/src/common_rtl.c
    ${SNESRECOMP_RUNNER_ROOT}/src/widescreen.c
    ${SNESRECOMP_RUNNER_ROOT}/src/recomp_hw.c
    ${SNESRECOMP_RUNNER_ROOT}/src/framedump.c
    ${SNESRECOMP_RUNNER_ROOT}/src/launcher.c
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
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/audio_shadow.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/dsp_shadow.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/msu1.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/color_lut.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/ppu.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/ppu_old.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/ws_shadow.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/snes.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/snes_other.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/spc.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/superfx.c
    # Interpreter-fallback tier (docs/MULTI_TIER.md): LakeSnes core + bridge.
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/interp816.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/interp_bridge.c
)

# The TCP debug server + emulator-oracle command handlers are a developer-only
# feature. debug_server.h provides static-inline no-op stubs when SNESRECOMP_TRACE
# is 0 (the default), so debug_server.c must only be compiled when tracing is on —
# otherwise the real definitions collide with the header stubs. Off by default for
# a normal playable build; opt in with -DSNESRECOMP_ENABLE_TRACE=ON.
option(SNESRECOMP_ENABLE_TRACE "Build the TCP debug server / observability rings" OFF)
if(SNESRECOMP_ENABLE_TRACE)
    list(APPEND SNESRECOMP_RUNNER_SOURCES
        ${SNESRECOMP_RUNNER_ROOT}/src/debug_server.c
    )
    if(EXISTS ${SNESRECOMP_RUNNER_ROOT}/src/emu_oracle_cmds.c)
        list(APPEND SNESRECOMP_RUNNER_SOURCES
            ${SNESRECOMP_RUNNER_ROOT}/src/emu_oracle_cmds.c
        )
    endif()
endif()

set(SNESRECOMP_RUNNER_LIBRARIES)
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

set(SNESRECOMP_RUNNER_INCLUDE_DIRS
    ${SNESRECOMP_RUNNER_ROOT}/src
    ${SNESRECOMP_RUNNER_ROOT}/src/snes
)
