# Headless SMW co-simulation A-side build (SNES_COSIM.md).
# DEV/DIAGNOSTICS ONLY. Builds smw_cosim: the SMW recomp runtime with NO SDL /
# OpenGL / host audio (harness_smw.c replaces main.c), plus the cosim engine.
#
# Configure (mingw + Ninja), from this dir:
#   cmake -S . -B build -G Ninja \
#     -DCMAKE_C_COMPILER=C:/msys64/mingw64/bin/gcc.exe \
#     -DCMAKE_MAKE_PROGRAM=C:/msys64/mingw64/bin/ninja.exe \
#     -DCMAKE_BUILD_TYPE=RelWithDebInfo
#   cmake --build build
cmake_minimum_required(VERSION 3.16)
project(smw_cosim C)
set(CMAKE_C_STANDARD 11)

# The worktree engine (this file lives in <worktree>/cosim) and the SMW game repo.
get_filename_component(SNESRECOMP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/.. ABSOLUTE)
set(SMW_ROOT "${SNESRECOMP_ROOT}/../SuperMarioWorldRecomp" CACHE PATH "SMW game repo")

set(SNES_COSIM ON CACHE BOOL "" FORCE)          # pull cosim.c/cosim_state.c + ws2_32
include(${SNESRECOMP_ROOT}/runner/runner.cmake)

file(GLOB SMW_GEN CONFIGURE_DEPENDS ${SMW_ROOT}/src/gen/*.c)
file(GLOB SMW_OVERRIDES CONFIGURE_DEPENDS ${SMW_ROOT}/overrides/*/*.c)
if(NOT SMW_GEN)
    message(FATAL_ERROR "no generated banks at ${SMW_ROOT}/src/gen — regen SMW first")
endif()

add_executable(smw_cosim
    ${SNESRECOMP_RUNNER_SOURCES}
    harness_smw.c
    harness_glue.c            # headless Die / RtlApuLock / debug WRAM-hook stubs
    # SMW game runtime (SDL/GL entry — main.c/opengl.c/glsl_shader.c/gl_core — EXCLUDED;
    # post_mortem.c EXCLUDED — DbgHelp crash backtrace, not needed headless)
    ${SMW_ROOT}/src/config.c
    ${SMW_ROOT}/src/smw_00.c
    ${SMW_ROOT}/src/gen_stubs.c
    ${SMW_ROOT}/src/smw_cpu_infra.c
    ${SMW_ROOT}/src/smw_rtl.c
    ${SMW_ROOT}/src/smw_spc_player.c
    ${SMW_GEN}
    ${SMW_OVERRIDES}
)
target_include_directories(smw_cosim PRIVATE
    ${SMW_ROOT} ${SMW_ROOT}/src ${SMW_ROOT}/recomp
    ${SNESRECOMP_RUNNER_INCLUDE_DIRS}
)
target_compile_definitions(smw_cosim PRIVATE
    SMW_DISABLE_LM=1
    SYSTEM_VOLUME_MIXER_AVAILABLE=0
    SNESRECOMP_TRACE=0        # no TCP debug server; the cosim server is separate
)
# SDL2 headers are pulled in for keycode/types (keybinds.c, config.h). We link
# SDL2 (NOT SDL2main) and define SDL_MAIN_HANDLED so SDL does not hijack main() —
# the harness owns main and NEVER calls SDL_Init / opens a window or audio
# device, so no host threads / audio sink spawn (Gate-1 determinism preserved).
target_compile_definitions(smw_cosim PRIVATE SDL_MAIN_HANDLED)
find_package(SDL2 QUIET)
if(SDL2_INCLUDE_DIRS)
    target_include_directories(smw_cosim PRIVATE ${SDL2_INCLUDE_DIRS})
endif()
target_include_directories(smw_cosim PRIVATE C:/msys64/mingw64/include/SDL2)
target_link_libraries(smw_cosim PRIVATE C:/msys64/mingw64/lib/libSDL2.dll.a)

# The recompiled C is machine-generated with K&R-style implicit cross-bank
# declarations; quiet + tolerate them exactly as the canonical build does.
if(NOT MSVC)
    target_compile_options(smw_cosim PRIVATE
        -w -Wno-implicit-function-declaration -Wno-error=implicit-function-declaration)
endif()

target_link_libraries(smw_cosim PRIVATE ${SNESRECOMP_RUNNER_LIBRARIES} m)
if(WIN32)
    target_link_libraries(smw_cosim PRIVATE ws2_32)
endif()

# ── Track A B-side: interp816 reference (smw_cosim_ref) ─────────────────────
# Explicit source list = runner DEVICE sources + interp816 + cosim + a headless
# accurate frame driver. Deliberately EXCLUDES the recomp runtime (common_rtl.c,
# cpu_state.c, gen banks, main.c) — the ref executes via interp816, not the
# recompiled C. Same device sources as the A-side => identical struct layouts =>
# the state hash compares directly.
set(RUNSNES ${SNESRECOMP_ROOT}/runner/src)
add_executable(smw_cosim_ref
    ref_driver.c
    ${RUNSNES}/cosim.c
    ${RUNSNES}/cosim_state.c
    ${RUNSNES}/audio_trace.c
    ${RUNSNES}/crc32.c
    ${RUNSNES}/sha256.c
    ${RUNSNES}/util.c
    ${RUNSNES}/snes/apu.c
    ${RUNSNES}/snes/cart.c
    ${RUNSNES}/snes/cpu.c
    ${RUNSNES}/snes/dma.c
    ${RUNSNES}/snes/dsp.c
    ${RUNSNES}/snes/ppu.c
    ${RUNSNES}/snes/ppu_old.c
    ${RUNSNES}/snes/snes.c
    ${RUNSNES}/snes/snes_other.c
    ${RUNSNES}/snes/spc.c
    ${RUNSNES}/snes/interp816.c
    ${RUNSNES}/snes/color_lut.c
)
target_include_directories(smw_cosim_ref PRIVATE ${RUNSNES} ${RUNSNES}/snes ${SMW_ROOT} ${SMW_ROOT}/src)
target_compile_definitions(smw_cosim_ref PRIVATE SNES_COSIM SNES_COSIM_REF SNESRECOMP_TRACE=0 SDL_MAIN_HANDLED)
if(NOT MSVC)
    target_compile_options(smw_cosim_ref PRIVATE -w -Wno-implicit-function-declaration)
endif()
target_link_libraries(smw_cosim_ref PRIVATE m)
if(WIN32)
    target_link_libraries(smw_cosim_ref PRIVATE ws2_32)
endif()
