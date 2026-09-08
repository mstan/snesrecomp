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

# Portable toolchain packs (retcomm-toolchains cmake-clang-v1) compile and link
# against a bundled sysroot -- clang.cfg passes --sysroot itself -- and their
# env.sh exports CMAKE_SYSROOT for the same directory. CMake does not read that
# variable from the environment, only from a -D or a toolchain file, so every
# find_* still searched the HOST: on a bare CI runner find_package(OpenGL) then
# failed even though the sysroot ships GL/gl.h and libGL.so, and on a developer
# machine it silently linked the host's GL instead of the sysroot's. A player's
# rebuild from a setup pack is the bare-machine case. Honour the pack's word.
if(NOT CMAKE_SYSROOT AND DEFINED ENV{CMAKE_SYSROOT} AND IS_DIRECTORY "$ENV{CMAKE_SYSROOT}")
    set(CMAKE_SYSROOT "$ENV{CMAKE_SYSROOT}")
    message(STATUS "snesrecomp: CMAKE_SYSROOT from environment: ${CMAKE_SYSROOT}")
    # The pack's sysroot carries legacy libGL.so (+ headers), not GLVND's
    # libOpenGL.so / libGLX.so dev links. FindOpenGL prefers GLVND and, not
    # finding it in the sysroot, falls through to the host -- which on a bare
    # runner has nothing, and on a developer box has a different distro's GL.
    # Legacy preference makes the sysroot the answer on every machine; the
    # binary still loads the player's own libGL.so.1 at runtime.
    if(NOT DEFINED OpenGL_GL_PREFERENCE)
        set(OpenGL_GL_PREFERENCE LEGACY)
    endif()
endif()

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

option(SNESRECOMP_STATIC_RUNTIME
    "On MinGW, link the C/C++ runtime statically so the executable is self-contained" ON)

option(SNESRECOMP_SDL3_FETCH
    "When no SDL3 package is found, build the pinned SDL3 release from source" ON)
set(SNESRECOMP_SDL3_FETCH_VERSION "3.4.10" CACHE STRING
    "SDL3 release built when SNESRECOMP_SDL3_FETCH kicks in")
set(SNESRECOMP_SDL3_FETCH_SHA256
    "12b34280415ec8418c864408b93d008a20a6530687ee613d60bfbd20411f2785"
    CACHE STRING "sha256 of the SDL3 release tarball")

function(snesrecomp_target_sdl target)
    if(_SNESRECOMP_SDL_BACKEND STREQUAL "SDL3")
        find_package(SDL3 CONFIG QUIET)
        if(NOT TARGET SDL3::SDL3 AND SNESRECOMP_SDL3_FETCH)
            message(STATUS
                "${target}: no SDL3 package found; building SDL3 "
                "${SNESRECOMP_SDL3_FETCH_VERSION} from source "
                "(-DSNESRECOMP_SDL3_FETCH=OFF to require an installed one)")
            include(FetchContent)
            # Static, so the finished executable carries its SDL and a
            # release zip does not have to ship a shared library beside it.
            set(SDL_SHARED OFF CACHE BOOL "" FORCE)
            set(SDL_STATIC ON CACHE BOOL "" FORCE)
            set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
            set(SDL_TESTS OFF CACHE BOOL "" FORCE)
            set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
            set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
            set(_snesrecomp_sdl3_url
                "https://github.com/libsdl-org/SDL/releases/download/release-${SNESRECOMP_SDL3_FETCH_VERSION}/SDL3-${SNESRECOMP_SDL3_FETCH_VERSION}.tar.gz")
            if(DEFINED SNESRECOMP_SDL3_SOURCE_DIR AND
               EXISTS "${SNESRECOMP_SDL3_SOURCE_DIR}/CMakeLists.txt")
                # A pre-fetched tree (CI caches one; curl --http1.1 is more
                # reliable against GitHub release assets than CMake's downloader).
                FetchContent_Declare(SDL3 SOURCE_DIR "${SNESRECOMP_SDL3_SOURCE_DIR}")
            else()
                FetchContent_Declare(SDL3
                    URL "${_snesrecomp_sdl3_url}"
                    URL_HASH "SHA256=${SNESRECOMP_SDL3_FETCH_SHA256}"
                    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
            endif()
            FetchContent_MakeAvailable(SDL3)
            unset(_snesrecomp_sdl3_url)
        endif()
        if(NOT TARGET SDL3::SDL3)
            message(FATAL_ERROR
                "${target}: SDL3 was not found. Install an SDL3 development "
                "package, pass -DSDL3_DIR=<dir with SDL3Config.cmake>, or leave "
                "SNESRECOMP_SDL3_FETCH=ON to build it from source.")
        endif()
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
    # MinGW (gcc or llvm-mingw clang) links libgcc/libstdc++ -- or libc++,
    # libunwind -- and winpthread as DLLs that live beside the COMPILER, not
    # beside the game. An executable that works in the build tree and fails to
    # start on a player's PC is the default outcome, and it is also what a
    # setup zip's own rebuild would produce: the relaunched binary lands in
    # build/, nowhere near any DLL the zip carried. Static linking makes the
    # executable self-contained on every path that produces one.
    if(WIN32 AND MINGW AND SNESRECOMP_STATIC_RUNTIME)
        target_link_options(${target} PRIVATE -static)
        message(STATUS "${target}: static MinGW runtime (-static)")
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
    ${SNESRECOMP_RUNNER_ROOT}/src/lobby/snes_netplay_auth.c
    ${SNESRECOMP_RUNNER_ROOT}/src/keybinds.c
    ${SNESRECOMP_RUNNER_ROOT}/src/snes_savestate_menu.c
    ${SNESRECOMP_RUNNER_ROOT}/src/cpu_state.c
    ${SNESRECOMP_RUNNER_ROOT}/src/cpu_trace.c
    ${SNESRECOMP_RUNNER_ROOT}/src/audio_trace.c
    ${SNESRECOMP_RUNNER_ROOT}/src/ppu_dma_trace.c
    ${SNESRECOMP_RUNNER_ROOT}/src/host_report.c
    ${SNESRECOMP_RUNNER_ROOT}/src/execution_mode.c
    ${SNESRECOMP_RUNNER_ROOT}/src/mod_audio.c
    ${SNESRECOMP_RUNNER_ROOT}/src/host_mesh.c
    ${SNESRECOMP_RUNNER_ROOT}/src/host_mesh_builder.c
    ${SNESRECOMP_RUNNER_ROOT}/src/guarded_patch.c
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
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/sdd1.c
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
    add_compile_definitions(SNESRECOMP_TRACE=1)
    # Compiling debug_server.c is only half of turning tracing on. The header
    # keys off SNESRECOMP_TRACE, not off the option, and defaults it to 0 — so
    # without this define every translation unit (debug_server.c included) sees
    # the no-op stubs, and debug_server.c's real definitions collide with the
    # stubs it just pulled in from its own header. The option was inert before
    # this: -DSNESRECOMP_ENABLE_TRACE=ON did not build, it only failed.
    add_compile_definitions(SNESRECOMP_TRACE=1)
    list(APPEND SNESRECOMP_RUNNER_SOURCES
        ${SNESRECOMP_RUNNER_ROOT}/src/debug_server.c
        ${SNESRECOMP_RUNNER_ROOT}/src/desktop/post_mortem.c
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
        ${SNESRECOMP_RUNNER_ROOT}/src/snes_text_xlate.cpp
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
include(${CMAKE_CURRENT_LIST_DIR}/lua.cmake)
if(NOT WIN32)
    # cx4.c synthesizes its internal data ROM with libm.
    list(APPEND SNESRECOMP_RUNNER_LIBRARIES m)
endif()
if(SNESRECOMP_ENABLE_TRACE AND WIN32)
    list(APPEND SNESRECOMP_RUNNER_LIBRARIES ws2_32 dbghelp)
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

# Runtime CPU/APU diagnostic helpers are useful for trace and co-sim, but are
# not part of emulated hardware. Keep their storage and hot-path updates out of
# production unless a build opts in explicitly.
option(SNESRECOMP_ENABLE_FUNC_SNAPSHOT
    "Capture function-boundary WRAM snapshots for debug-server bisection"
    ${SNESRECOMP_ENABLE_TRACE})
option(SNESRECOMP_ENABLE_STACK_BALANCE_DIAGNOSTICS
    "Collect per-function stack-balance diagnostics"
    ${SNESRECOMP_ENABLE_TRACE})
option(SNESRECOMP_ENABLE_BOOT_WATCHDOG_DIAGNOSTICS
    "Sample recomp stack during frame-0 boot stalls"
    ${SNESRECOMP_ENABLE_TRACE})
option(SNESRECOMP_ENABLE_CPU_HW_DIAGNOSTICS
    "Collect CPU hardware-register touch diagnostics"
    ${SNESRECOMP_ENABLE_TRACE})
option(SNESRECOMP_ENABLE_SPC_DIAGNOSTICS
    "Collect SPC PC histogram and port-write diagnostics"
    ${SNESRECOMP_ENABLE_TRACE})
if(SNES_COSIM OR SNESRECOMP_ENABLE_TRACE)
    set(_SNESRECOMP_FUNC_SNAPSHOT 1)
    set(_SNESRECOMP_STACK_BALANCE_DIAGNOSTICS 1)
    set(_SNESRECOMP_BOOT_WATCHDOG_DIAGNOSTICS 1)
    set(_SNESRECOMP_CPU_HW_DIAGNOSTICS 1)
    set(_SNESRECOMP_SPC_DIAGNOSTICS 1)
else()
    set(_SNESRECOMP_FUNC_SNAPSHOT ${SNESRECOMP_ENABLE_FUNC_SNAPSHOT})
    set(_SNESRECOMP_STACK_BALANCE_DIAGNOSTICS
        ${SNESRECOMP_ENABLE_STACK_BALANCE_DIAGNOSTICS})
    set(_SNESRECOMP_BOOT_WATCHDOG_DIAGNOSTICS
        ${SNESRECOMP_ENABLE_BOOT_WATCHDOG_DIAGNOSTICS})
    set(_SNESRECOMP_CPU_HW_DIAGNOSTICS
        ${SNESRECOMP_ENABLE_CPU_HW_DIAGNOSTICS})
    set(_SNESRECOMP_SPC_DIAGNOSTICS ${SNESRECOMP_ENABLE_SPC_DIAGNOSTICS})
endif()
if(_SNESRECOMP_FUNC_SNAPSHOT)
    set(_SNESRECOMP_FUNC_SNAPSHOT 1)
else()
    set(_SNESRECOMP_FUNC_SNAPSHOT 0)
endif()
if(_SNESRECOMP_STACK_BALANCE_DIAGNOSTICS)
    set(_SNESRECOMP_STACK_BALANCE_DIAGNOSTICS 1)
else()
    set(_SNESRECOMP_STACK_BALANCE_DIAGNOSTICS 0)
endif()
if(_SNESRECOMP_BOOT_WATCHDOG_DIAGNOSTICS)
    set(_SNESRECOMP_BOOT_WATCHDOG_DIAGNOSTICS 1)
else()
    set(_SNESRECOMP_BOOT_WATCHDOG_DIAGNOSTICS 0)
endif()
if(_SNESRECOMP_CPU_HW_DIAGNOSTICS)
    set(_SNESRECOMP_CPU_HW_DIAGNOSTICS 1)
else()
    set(_SNESRECOMP_CPU_HW_DIAGNOSTICS 0)
endif()
if(_SNESRECOMP_SPC_DIAGNOSTICS)
    set(_SNESRECOMP_SPC_DIAGNOSTICS 1)
else()
    set(_SNESRECOMP_SPC_DIAGNOSTICS 0)
endif()
message(STATUS
    "SNES runtime diagnostics: func_snapshot=${_SNESRECOMP_FUNC_SNAPSHOT}; "
    "stack_balance=${_SNESRECOMP_STACK_BALANCE_DIAGNOSTICS}; "
    "boot_watchdog=${_SNESRECOMP_BOOT_WATCHDOG_DIAGNOSTICS}; "
    "cpu_hw=${_SNESRECOMP_CPU_HW_DIAGNOSTICS}; "
    "spc=${_SNESRECOMP_SPC_DIAGNOSTICS}")
set_property(SOURCE
    ${SNESRECOMP_RUNNER_ROOT}/src/common_cpu_infra.c
    APPEND PROPERTY COMPILE_DEFINITIONS
    SNESRECOMP_FUNC_SNAPSHOT=${_SNESRECOMP_FUNC_SNAPSHOT}
    SNESRECOMP_STACK_BALANCE_DIAGNOSTICS=${_SNESRECOMP_STACK_BALANCE_DIAGNOSTICS}
    SNESRECOMP_BOOT_WATCHDOG_DIAGNOSTICS=${_SNESRECOMP_BOOT_WATCHDOG_DIAGNOSTICS})
set_property(SOURCE
    ${SNESRECOMP_RUNNER_ROOT}/src/cpu_state.c
    APPEND PROPERTY COMPILE_DEFINITIONS
    SNESRECOMP_CPU_HW_DIAGNOSTICS=${_SNESRECOMP_CPU_HW_DIAGNOSTICS})
set_property(SOURCE
    ${SNESRECOMP_RUNNER_ROOT}/src/snes/apu.c
    APPEND PROPERTY COMPILE_DEFINITIONS
    SNESRECOMP_SPC_DIAGNOSTICS=${_SNESRECOMP_SPC_DIAGNOSTICS})
unset(_SNESRECOMP_FUNC_SNAPSHOT)
unset(_SNESRECOMP_STACK_BALANCE_DIAGNOSTICS)
unset(_SNESRECOMP_BOOT_WATCHDOG_DIAGNOSTICS)
unset(_SNESRECOMP_CPU_HW_DIAGNOSTICS)
unset(_SNESRECOMP_SPC_DIAGNOSTICS)

# Audio counters are functional production telemetry used by the runtime and
# reports. The large PCM/event/snapshot histories are forensic diagnostics:
# useful in trace builds, expensive in the sample/DSP hot path, and large in
# static storage. Co-simulation and trace builds force the historical full
# rings. Production defaults to SMALL after serialized title A/B acceptance,
# and can still be tested with:
#
#   -DSNESRECOMP_AUDIO_TRACE_HISTORY=COUNTERS
#   -DSNESRECOMP_AUDIO_TRACE_HISTORY=SMALL
#   -DSNESRECOMP_AUDIO_TRACE_HISTORY=FULL
#   -DSNESRECOMP_AUDIO_TRACE_HISTORY=RESERVED
#
# SMALL retains about 2 s of native PCM, 16K events, and 512 one-second
# snapshots (~8.5 min). FULL retains about 131 s of PCM, 512K events, and
# 4096 snapshots (~68 min). SMALL drop-run fragmentation follows actual event
# retention; that is debug telemetry fidelity, not emulated audio behavior.
# RESERVED keeps full storage allocated but skips hot-path history writes,
# which isolates layout/footprint effects from write bandwidth.
set(SNESRECOMP_AUDIO_TRACE_HISTORY "SMALL" CACHE STRING
    "Audio trace history retention (COUNTERS, SMALL, FULL, RESERVED)")
set_property(CACHE SNESRECOMP_AUDIO_TRACE_HISTORY PROPERTY STRINGS
    COUNTERS SMALL FULL RESERVED)
string(TOUPPER "${SNESRECOMP_AUDIO_TRACE_HISTORY}" _SNESRECOMP_AUDIO_HISTORY)
if(SNES_COSIM OR SNESRECOMP_ENABLE_TRACE)
    set(_SNESRECOMP_AUDIO_HISTORY "FULL")
endif()
if(_SNESRECOMP_AUDIO_HISTORY STREQUAL "COUNTERS")
    set(_SNESRECOMP_AUDIO_HISTORY_VALUE 0)
elseif(_SNESRECOMP_AUDIO_HISTORY STREQUAL "SMALL")
    set(_SNESRECOMP_AUDIO_HISTORY_VALUE 1)
elseif(_SNESRECOMP_AUDIO_HISTORY STREQUAL "FULL")
    set(_SNESRECOMP_AUDIO_HISTORY_VALUE 2)
elseif(_SNESRECOMP_AUDIO_HISTORY STREQUAL "RESERVED")
    set(_SNESRECOMP_AUDIO_HISTORY_VALUE 3)
else()
    message(FATAL_ERROR
        "SNESRECOMP_AUDIO_TRACE_HISTORY must be COUNTERS, SMALL, FULL, "
        "or RESERVED (got '${SNESRECOMP_AUDIO_TRACE_HISTORY}')")
endif()
message(STATUS
    "SNES audio trace history: ${_SNESRECOMP_AUDIO_HISTORY}")
set_property(SOURCE
    ${SNESRECOMP_RUNNER_ROOT}/src/audio_trace.c
    APPEND PROPERTY COMPILE_DEFINITIONS
    SNESRECOMP_AUDIO_TRACE_HISTORY=${_SNESRECOMP_AUDIO_HISTORY_VALUE})
unset(_SNESRECOMP_AUDIO_HISTORY)
unset(_SNESRECOMP_AUDIO_HISTORY_VALUE)

set(SNESRECOMP_RUNNER_INCLUDE_DIRS
    ${SNESRECOMP_RUNNER_ROOT}/src
    ${SNESRECOMP_RUNNER_ROOT}/src/snes
)

# ── Directories the game loads relative to its executable ───────────────────
#
# snesrecomp_target_stage_dir(<target> <source_dir> <dest_relative>)
#
# Copies <source_dir> to $<TARGET_FILE_DIR:target>/<dest_relative> every time
# <target> is built. mods/, translations/ and the like are read from beside
# the executable at runtime, so a binary without them beside it runs without
# them -- silently, for a mods catalog.
#
# POST_BUILD on the executable rather than a separate ALL target, because the
# executable is the one thing every build path agrees on: `cmake --build
# --target <exe>` from CI, from a developer, and from the launcher's own
# Generate & rebuild all produce it. A sibling ALL target is skipped by every
# one of those that names the target, which is how a setup pack rebuilt on a
# player's machine came up with an empty Mods page: the pack root beside the
# setup host had mods/, the freshly built build/<exe> did not.
function(snesrecomp_target_stage_dir target source_dir dest_rel)
    if(NOT IS_DIRECTORY "${source_dir}")
        message(FATAL_ERROR
            "snesrecomp_target_stage_dir(${target}): ${source_dir} is not a directory")
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${source_dir}" "$<TARGET_FILE_DIR:${target}>/${dest_rel}"
        COMMENT "${target}: staging ${dest_rel}/ beside the executable"
        VERBATIM)
endfunction()

# ── The mod catalog — THE FRAMEWORK OWNS THE LAYOUT ─────────────────────────
#
# snesrecomp_target_mod_catalog(<target> <preloaded_dir>|NONE)
#
# A title says WHERE its packages live and never HOW they are laid out beside
# the executable. `<preloaded_dir>` is the directory shaped like
# <repo>/mods/preloaded, holding packages/<id>/<version>/manifest.toml.
#
# WHY THIS REPLACES A HAND-WRITTEN COPY. Every mod-carrying SNES title spelled
# the destination itself:
#
#     snesrecomp_target_stage_dir(<target> "${CMAKE_SOURCE_DIR}/mods" mods)
#
# which is the same shape psxrecomp had to withdraw after renaming its staged
# directory broke five titles at once, silently, because the path was a string
# in six repositories (psxrecomp runtime.cmake, "Mod catalog staging"). The
# SNES failure did not even need a rename: a title that simply never wrote the
# line shipped a binary whose Mods page is empty, and nothing in a configure, a
# build or a test says so. Measured across this workspace: of eight SNES titles
# with a package catalog or a mods/ tree, three staged nothing at all.
#
# Two guards, so "the title forgot" stops being a thing that can happen:
#
#   * configure time — a project with packages under mods/preloaded/packages
#     that never declares them is a hard error, naming the call to add. An
#     EMPTY packages/ (the scaffold's initial state) configures fine, because
#     a title that has not written a mod yet must still build.
#   * build time — snes_check_mod_catalog.cmake runs after the staging copy and
#     fails the build if a declared package did not arrive. A staging step that
#     half-worked is otherwise indistinguishable from a title with no mods.
#
# It also refuses a catalog that no loader will read: packages present with
# SNESRECOMP_ENABLE_MODS OFF means the files ship and the Mods page stays dark,
# which is the same empty page by a different route.
#
# POST_BUILD on the executable, for the reason snesrecomp_target_stage_dir
# gives above: a sibling ALL target is skipped by `cmake --build --target <exe>`,
# which is how the launcher's own Generate & rebuild produces a game.

# Immediate subdirectory names of `root` (package ids), sorted. CONFIGURE_DEPENDS
# so that creating the first package re-runs configure and trips the guard
# rather than waiting for someone to notice at release time.
function(_snesrecomp_package_ids root out_var)
    set(_ids "")
    file(GLOB _entries CONFIGURE_DEPENDS LIST_DIRECTORIES true "${root}/*")
    foreach(_e IN LISTS _entries)
        if(IS_DIRECTORY "${_e}")
            get_filename_component(_n "${_e}" NAME)
            list(APPEND _ids "${_n}")
        endif()
    endforeach()
    list(SORT _ids)
    set(${out_var} "${_ids}" PARENT_SCOPE)
endfunction()

# The one place the staged layout is written down.
set(SNESRECOMP_MOD_CATALOG_DEST "mods/preloaded/packages")

function(snesrecomp_target_mod_catalog target preloaded_dir)
    if(NOT TARGET ${target})
        message(FATAL_ERROR
            "snesrecomp_target_mod_catalog: no such target '${target}'")
    endif()
    set_property(GLOBAL APPEND PROPERTY SNESRECOMP_MOD_CATALOG_TARGETS "${target}")

    if(preloaded_dir STREQUAL "NONE")
        # An explicit "this title ships no catalog". Recorded, so the guard
        # below can tell it apart from a title that never called this at all.
        return()
    endif()
    if(NOT IS_DIRECTORY "${preloaded_dir}")
        message(FATAL_ERROR
            "snesrecomp_target_mod_catalog(${target}): ${preloaded_dir} is not "
            "a directory. Pass the directory shaped like "
            "<repo>/mods/preloaded, or the literal NONE.")
    endif()

    set(_pkg_src "${preloaded_dir}/packages")
    set(_ids "")
    if(IS_DIRECTORY "${_pkg_src}")
        _snesrecomp_package_ids("${_pkg_src}" _ids)
    endif()

    if(_ids AND NOT SNESRECOMP_ENABLE_MODS)
        list(JOIN _ids "\n    " _pretty)
        message(FATAL_ERROR
            "This project ships a mod catalog:\n"
            "    ${_pretty}\n"
            "but SNESRECOMP_ENABLE_MODS is OFF, so the loader is not compiled "
            "and the Mods page cannot show any of it. The packages would be "
            "copied beside the executable and read by nothing.\n\n"
            "Set it before including runner.cmake:\n\n"
            "    set(SNESRECOMP_ENABLE_MODS ON CACHE BOOL \"\" FORCE)\n")
    endif()

    # Nothing to stage yet: a scaffold with an empty packages/ is a valid
    # state, and copy_directory on a missing source is a build error.
    if(NOT IS_DIRECTORY "${_pkg_src}")
        return()
    endif()

    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${_pkg_src}"
            "$<TARGET_FILE_DIR:${target}>/${SNESRECOMP_MOD_CATALOG_DEST}"
        COMMENT "${target}: staging the mod catalog into ${SNESRECOMP_MOD_CATALOG_DEST}/"
        VERBATIM)

    if(_ids)
        # Verify rather than assume. copy_directory reports its own failures,
        # but a catalog that arrives incomplete (a package removed from the
        # source tree between configure and build, a destination someone else's
        # POST_BUILD wiped) looks exactly like a title with fewer mods.
        list(JOIN _ids "|" _ids_arg)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                "-DSNES_MOD_DEST=$<TARGET_FILE_DIR:${target}>/${SNESRECOMP_MOD_CATALOG_DEST}"
                "-DSNES_MOD_IDS=${_ids_arg}"
                "-DSNES_MOD_TARGET=${target}"
                -P "${SNESRECOMP_RUNNER_ROOT}/snes_check_mod_catalog.cmake"
            COMMENT "${target}: verifying the staged mod catalog"
            VERBATIM)
    endif()
endfunction()

# Runs at the end of the directory that included runner.cmake, i.e. after the
# title has had every chance to declare its catalog.
function(_snesrecomp_finalize_mod_catalog_guard)
    get_property(_declared GLOBAL PROPERTY SNESRECOMP_MOD_CATALOG_TARGETS)
    if(_declared)
        return()
    endif()
    set(_ids "")
    if(IS_DIRECTORY "${CMAKE_SOURCE_DIR}/mods/preloaded/packages")
        _snesrecomp_package_ids(
            "${CMAKE_SOURCE_DIR}/mods/preloaded/packages" _ids)
    endif()
    if(NOT _ids)
        return()
    endif()
    list(JOIN _ids "\n    " _pretty)
    message(FATAL_ERROR
        "This project has a mod catalog at "
        "${CMAKE_SOURCE_DIR}/mods/preloaded/packages:\n"
        "    ${_pretty}\n"
        "but no target declared it, so those packages would never be staged "
        "beside the executable and the Mods page would be empty in every "
        "build and every release zip.\n\n"
        "Add, after the target exists:\n\n"
        "    snesrecomp_target_mod_catalog(<target>\n"
        "        \"\${CMAKE_SOURCE_DIR}/mods/preloaded\")\n\n"
        "and DELETE any snesrecomp_target_stage_dir(... mods) or "
        "add_custom_command(... copy_directory ... /mods) block: the framework "
        "owns the destination layout now, so a future rename touches "
        "runner.cmake and nothing else. Pass NONE instead of a directory if "
        "this title genuinely ships no catalog.")
endfunction()

# cmake_language(DEFER) is 3.19+. Titles scaffolded against 3.16 still build on
# a newer cmake binary, so gate on what is running rather than on the policy
# version -- and when it is genuinely unavailable, the per-call checks above
# still apply; only the "never called it at all" guard is lost.
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.19)
    cmake_language(DEFER CALL _snesrecomp_finalize_mod_catalog_guard)
endif()

# ── ROM identity ────────────────────────────────────────────────────────────
#
# snesrecomp_rom_identity(<target> <identity_file>)
#
# rom_identity.txt is the one place a project's ROM digests live. This turns
# it into snesrecomp_rom_identity.h (SNESRECOMP_ROM_EXPECTED_SHA256 and
# friends, see cmake/rom_identity.h.in) at configure time and puts the header
# on <target>'s include path, so the host compiles the digests in and a
# shipped binary carries no file it can be separated from. tools/regen.sh and
# the release workflow read the same text file through tools/rom_identity.py,
# which emits an identical header for CI (CI cannot configure a project whose
# src/gen is empty); tests/test_rom_identity.py pins the two emitters together.
#
# Format: `key = value`, one per line, `#` comments, optional double quotes.
# Keys: display_name, rom_file, expected_crc32, expected_sha256, mapping,
# region, game_id (the id mod manifests target). A missing key expands to "", which every consumer reads as
# "cannot verify" rather than "verified". Editing the file re-runs configure.
function(snesrecomp_rom_identity target identity_file)
    if(NOT EXISTS "${identity_file}")
        message(FATAL_ERROR
            "snesrecomp_rom_identity(${target}): ${identity_file} does not exist.\n"
            "It is the project's ROM identity (digests, filename, mapping); the "
            "scaffolder writes it as rom_identity.txt at the project root.")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${identity_file}")

    foreach(_field display_name rom_file expected_crc32 expected_sha256 mapping region game_id)
        set(_id_${_field} "")
    endforeach()

    # Not file(STRINGS): a value containing ';' would be split into two lines.
    file(READ "${identity_file}" _content)
    string(REPLACE ";" "\;" _content "${_content}")
    string(REGEX REPLACE "\r?\n" ";" _lines "${_content}")
    foreach(_line IN LISTS _lines)
        string(STRIP "${_line}" _line)
        if(_line STREQUAL "" OR _line MATCHES "^#")
            continue()
        endif()
        if(NOT _line MATCHES "^([A-Za-z0-9_]+)[ \t]*=[ \t]*(.*)$")
            message(WARNING "${identity_file}: ignoring unparsable line: ${_line}")
            continue()
        endif()
        set(_key "${CMAKE_MATCH_1}")
        string(STRIP "${CMAKE_MATCH_2}" _value)
        if(_value MATCHES "^\"(.*)\"$")
            set(_value "${CMAKE_MATCH_1}")
        endif()
        string(TOLOWER "${_key}" _key)
        set(_id_${_key} "${_value}")
    endforeach()

    # The macro names are the contract with tools/rom_identity.py and every
    # host that includes the header; spelled out rather than derived so the
    # one irregular name (rom_file -> SNESRECOMP_ROM_FILE) cannot drift.
    set(SNESRECOMP_ROM_DISPLAY_NAME    "${_id_display_name}")
    set(SNESRECOMP_ROM_FILE            "${_id_rom_file}")
    set(SNESRECOMP_ROM_EXPECTED_CRC32  "${_id_expected_crc32}")
    set(SNESRECOMP_ROM_EXPECTED_SHA256 "${_id_expected_sha256}")
    set(SNESRECOMP_ROM_MAPPING         "${_id_mapping}")
    set(SNESRECOMP_ROM_REGION          "${_id_region}")
    set(SNESRECOMP_ROM_GAME_ID         "${_id_game_id}")

    if(SNESRECOMP_ROM_EXPECTED_SHA256 STREQUAL "" AND SNESRECOMP_ROM_EXPECTED_CRC32 STREQUAL "")
        message(WARNING
            "${identity_file}: sets neither expected_sha256 nor expected_crc32 -- "
            "this build cannot verify the ROM it is handed.")
    endif()

    set(SNESRECOMP_ROM_IDENTITY_SOURCE "${identity_file}")
    set(_out_dir "${CMAKE_CURRENT_BINARY_DIR}/snesrecomp_rom_identity")
    configure_file("${SNESRECOMP_RUNNER_ROOT}/cmake/rom_identity.h.in"
                   "${_out_dir}/snesrecomp_rom_identity.h" @ONLY)
    target_include_directories(${target} PRIVATE "${_out_dir}")
endfunction()

# ── Generated code, or the setup host that stands in for it ────────────────
#
# src/gen/*.c is recompiler output derived from ROM bytes: never committed,
# never in CI, never in a release zip. That leaves exactly one honest way to
# ship a binary without it -- a SETUP HOST: the same executable, built from
# the runner and the hand-written host sources, whose only job is to open the
# launcher's first-run wizard, take the player's own ROM, run the recompiler
# locally, rebuild, and relaunch into the real game.
#
# It is not a stub build. The dispatch tables it links are empty on purpose
# (setup_host_dispatch.c), and SnesInit() refuses to boot a guest at all when
# SNESRECOMP_SETUP_HOST is defined -- so nothing in it can ever run guest code
# with an invented result. Either the wizard produces a real build, or the
# player is told exactly what is missing.
option(SNESRECOMP_SETUP_HOST
    "Build the host without recompiled code: a setup binary whose only path is Generate & rebuild"
    OFF)

# snesrecomp_target_generated_code(<target> <gen_dir>)
#
# Adds the generated C under <gen_dir> to <target>, or -- when it is absent
# and SNESRECOMP_SETUP_HOST is ON -- the setup-host dispatch tables. A missing
# gen directory with the option OFF is still a hard configure error, because
# that is a developer who forgot to regenerate, and a silent fallback there
# would hand them a binary that cannot play and does not say why.
function(snesrecomp_target_generated_code target gen_dir)
    file(GLOB _gen_sources CONFIGURE_DEPENDS "${gen_dir}/*.c")
    if(_gen_sources)
        if(SNESRECOMP_SETUP_HOST)
            # Asked for a setup host while generated C is present: the two
            # are contradictory, and quietly picking one hides a stale
            # src/gen from whoever is packaging.
            message(FATAL_ERROR
                "SNESRECOMP_SETUP_HOST=ON but ${gen_dir} contains generated C.\n"
                "A setup host must be built from a tree WITHOUT generated code, "
                "so the zip cannot carry ROM-derived output by accident. Remove "
                "${gen_dir}/*.c (and recomp/funcs.h) or configure without the option.")
        endif()
        target_sources(${target} PRIVATE ${_gen_sources})
        # Generated C is machine output: it is not held to the runner's
        # warning bar, and treating it as such buries real warnings from the
        # hand-written host code.
        if(NOT MSVC)
            set_source_files_properties(${_gen_sources} PROPERTIES
                COMPILE_OPTIONS "-w")
        endif()
        target_compile_definitions(${target} PRIVATE SNESRECOMP_HAS_GENERATED_CODE=1)
        list(LENGTH _gen_sources _n)
        message(STATUS "${target}: ${_n} generated translation unit(s) from ${gen_dir}")
        return()
    endif()

    if(NOT SNESRECOMP_SETUP_HOST)
        message(FATAL_ERROR
            "${gen_dir} is empty -- run `bash tools/regen.sh` with your verified "
            "ROM before building.\n"
            "To build a SETUP HOST instead (a binary that regenerates on the "
            "player's machine), configure with -DSNESRECOMP_SETUP_HOST=ON.")
    endif()

    target_sources(${target} PRIVATE
        ${SNESRECOMP_RUNNER_ROOT}/src/setup_host_dispatch.c)
    target_compile_definitions(${target} PRIVATE SNESRECOMP_SETUP_HOST=1)
    message(STATUS
        "${target}: SETUP HOST -- no recompiled code; the launcher's Generate & "
        "rebuild wizard is the only path forward in this binary")
endfunction()

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
# RetroPortingToolKit/recomp-ui as a repo-root submodule and call recomp_target_launcher_ui()
# themselves (see docs/LAUNCHER_DESIGN.md).
