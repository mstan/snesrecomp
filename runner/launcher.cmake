# launcher.cmake — optional RmlUi pre-boot launcher for snesrecomp games.
#
# Builds vendored lib/RmlUi + lib/freetype (static), compiles the C++ launcher
# GUI + SDL/GL3 backends, defines SNESRECOMP_LAUNCHER=1, and copies assets next
# to the executable. Mirrors psxrecomp/runtime/runtime.cmake's PSX_LAUNCHER path
# and lib/CMakeLists.txt's dependency pins.
#
# Usage (after include(runner.cmake) and add_executable(...)):
#
#   snesrecomp_enable_launcher(MyGame)
#
# Opt out at configure time:
#   cmake -DSNESRECOMP_LAUNCHER=OFF ...

if(NOT SNESRECOMP_LIB_ROOT)
    get_filename_component(SNESRECOMP_LIB_ROOT
        "${CMAKE_CURRENT_LIST_DIR}/../lib" ABSOLUTE)
endif()

option(SNESRECOMP_LAUNCHER
    "Build the integrated RmlUi pre-boot launcher UI" ON)

# Build the vendored RmlUi + FreeType once per CMake project (idempotent).
function(snesrecomp_ensure_launcher_libs)
    if(TARGET rmlui_core)
        return()
    endif()
    if(NOT EXISTS "${SNESRECOMP_LIB_ROOT}/RmlUi/CMakeLists.txt" OR
       NOT EXISTS "${SNESRECOMP_LIB_ROOT}/freetype/CMakeLists.txt")
        message(FATAL_ERROR
            "SNESRECOMP_LAUNCHER=ON but lib/RmlUi or lib/freetype is missing.\n"
            "Run: git submodule update --init --recursive lib/RmlUi lib/freetype")
    endif()

    set(BUILD_SHARED_LIBS OFF)

    set(FT_DISABLE_ZLIB     TRUE CACHE BOOL "" FORCE)
    set(FT_DISABLE_BZIP2    TRUE CACHE BOOL "" FORCE)
    set(FT_DISABLE_PNG      TRUE CACHE BOOL "" FORCE)
    set(FT_DISABLE_HARFBUZZ TRUE CACHE BOOL "" FORCE)
    set(FT_DISABLE_BROTLI   TRUE CACHE BOOL "" FORCE)
    add_subdirectory("${SNESRECOMP_LIB_ROOT}/freetype"
                     "${CMAKE_BINARY_DIR}/_deps/freetype-build" EXCLUDE_FROM_ALL)
    if(NOT TARGET Freetype::Freetype)
        add_library(Freetype::Freetype ALIAS freetype)
    endif()

    set(RMLUI_SAMPLES             OFF        CACHE BOOL   "" FORCE)
    set(RMLUI_FONT_ENGINE         "freetype" CACHE STRING "" FORCE)
    set(RMLUI_PRECOMPILED_HEADERS OFF        CACHE BOOL   "" FORCE)
    set(RMLUI_THIRDPARTY_CONTAINERS OFF      CACHE BOOL   "" FORCE)
    add_subdirectory("${SNESRECOMP_LIB_ROOT}/RmlUi"
                     "${CMAKE_BINARY_DIR}/_deps/RmlUi-build" EXCLUDE_FROM_ALL)
endfunction()

# MotK WebSocket lobby client (runner/src/lobby). Needed by snes_netplay ICE
# signalling and by games that use recomp-ui (RECOMP_LAUNCHER) without the
# legacy RmlUi snesrecomp_enable_launcher path.
function(snesrecomp_enable_lobby target)
    if(NOT TARGET ${target})
        message(FATAL_ERROR
            "snesrecomp_enable_lobby: '${target}' is not a CMake target. "
            "Call this after add_executable(${target} ...).")
    endif()
    # Idempotent — RmlUi launcher and recomp-ui games may both request it.
    get_target_property(_already ${target} SNESRECOMP_LOBBY_ENABLED)
    if(_already)
        return()
    endif()
    set_target_properties(${target} PROPERTIES SNESRECOMP_LOBBY_ENABLED 1)

    set(_lobby_root "${SNESRECOMP_RUNNER_ROOT}/src/lobby")
    set(_lobby_ws_dir "${_lobby_root}/ws")

    target_sources(${target} PRIVATE ${_lobby_root}/snes_lobby_client.c)
    target_include_directories(${target} PRIVATE ${_lobby_root})

    if(EXISTS "${_lobby_ws_dir}/rnet_ws.c" AND EXISTS "${_lobby_ws_dir}/rnet_sha1.c")
        target_sources(${target} PRIVATE
            ${_lobby_ws_dir}/rnet_ws.c
            ${_lobby_ws_dir}/rnet_sha1.c)
        target_include_directories(${target} PRIVATE ${_lobby_ws_dir})
        target_compile_definitions(${target} PRIVATE SNES_HAS_LOBBY_CLIENT=1)
        message(STATUS "snesrecomp lobby client enabled (${target})")
    else()
        message(STATUS
            "snesrecomp lobby stubs only for ${target} (ws helpers missing)")
    endif()

    if(WIN32 OR MINGW)
        target_link_libraries(${target} PRIVATE ws2_32)
    endif()
endfunction()

# Link the RmlUi launcher into a game executable.
function(snesrecomp_enable_launcher target)
    if(NOT SNESRECOMP_LAUNCHER)
        message(STATUS "snesrecomp launcher disabled (SNESRECOMP_LAUNCHER=OFF)")
        return()
    endif()
    if(NOT TARGET ${target})
        message(FATAL_ERROR
            "snesrecomp_enable_launcher: '${target}' is not a CMake target. "
            "Call this after add_executable(${target} ...).")
    endif()

    snesrecomp_ensure_launcher_libs()
    snesrecomp_enable_lobby(${target})

    set(_launcher_root "${SNESRECOMP_RUNNER_ROOT}/src/launcher")

    target_sources(${target} PRIVATE
        ${_launcher_root}/launcher_gui.cpp
        ${SNESRECOMP_LIB_ROOT}/RmlUi/Backends/RmlUi_Platform_SDL.cpp
        ${SNESRECOMP_LIB_ROOT}/RmlUi/Backends/RmlUi_Renderer_GL3.cpp
    )

    # Vendored GL3 backend uses std::all_of without <algorithm> on some GCC.
    if(NOT MSVC)
        set_source_files_properties(
            ${SNESRECOMP_LIB_ROOT}/RmlUi/Backends/RmlUi_Renderer_GL3.cpp
            PROPERTIES COMPILE_OPTIONS "-include;algorithm")
    endif()

    target_include_directories(${target} PRIVATE
        ${_launcher_root}
        ${SNESRECOMP_LIB_ROOT}/RmlUi/Include
        ${SNESRECOMP_LIB_ROOT}/RmlUi/Backends
    )
    target_link_libraries(${target} PRIVATE RmlUi::Core)
    target_compile_definitions(${target} PRIVATE SNESRECOMP_LAUNCHER=1)

    # Ensure C++ linkage (game projects may declare project(... C) only).
    set_target_properties(${target} PROPERTIES LINKER_LANGUAGE CXX)
    if(NOT MSVC)
        target_compile_features(${target} PRIVATE cxx_std_17)
    endif()

    if(WIN32 OR MINGW)
        target_link_libraries(${target} PRIVATE comdlg32 ole32 ws2_32)
    endif()

    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${_launcher_root}/assets"
            "$<TARGET_FILE_DIR:${target}>/launcher"
        COMMENT "Copying snesrecomp launcher assets next to ${target}")
endfunction()
