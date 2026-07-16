# launcher_ng — next-gen launcher (Dear ImGui) for the recomp ecosystem.
#
# Default build is SDL2 — it matches the game runtime, so the launcher links
# in-process behind snes_launcher_run_window with zero migration. Pass
# -DLNG_SDL3=ON for the follow-up SDL3 build that adds Wayland fractional
# scaling (that path FetchContents SDL3 and also builds the Clay reference
# backend).
#
# Build (mingw + ninja), SDL2 default:
#   cmake -G Ninja -S runner/src/launcher_ng -B runner/src/launcher_ng/build
#   cmake --build runner/src/launcher_ng/build
# SDL3 follow-up:
#   cmake -G Ninja -DLNG_SDL3=ON -S ... -B .../build-sdl3

cmake_minimum_required(VERSION 3.24)
project(launcher_ng C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)   # Clay's C++ macro path (SDL3 build) requires C++20
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(LNG_SDL3 "Build against SDL3 (adds Wayland fractional scaling + Clay backend)" OFF)

# Our real main() is the entry point; don't let SDL redirect main()->SDL_main
# (which would require linking SDL2main / SDL3main). Matches how the MMX game
# links SDL on mingw.
add_compile_definitions(SDL_MAIN_HANDLED)

include(FetchContent)

set(LNG_DIR         ${CMAKE_CURRENT_SOURCE_DIR})
set(RUNNER_SRC      ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(LAUNCHER_ASSETS ${RUNNER_SRC}/launcher/assets)
set(MMX_BOXART      ${CMAKE_CURRENT_SOURCE_DIR}/../../../../MegamanXRecomp/recomp/launcher/boxart.tga)

# ---- SDL: SDL2 (system, matches the game) or SDL3 (fetched) --------------------
if(LNG_SDL3)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON  CACHE BOOL "" FORCE)
    set(SDL_TEST   OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL
        GIT_TAG release-3.4.12 GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(SDL3)
    set(LNG_SDL_LIB SDL3::SDL3)
    set(LNG_IMGUI_SDL_BACKEND imgui_impl_sdl3)
else()
    find_package(SDL2 REQUIRED)
    set(LNG_SDL_LIB SDL2::SDL2)
    set(LNG_IMGUI_SDL_BACKEND imgui_impl_sdl2)
endif()

# ---- Dear ImGui (MIT) : no CMakeLists, so build our own static lib -------------
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui
    GIT_TAG v1.91.9b GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/${LNG_IMGUI_SDL_BACKEND}.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC ${LNG_SDL_LIB})
if(LNG_SDL3)
    target_compile_definitions(imgui PUBLIC LNG_SDL3)
endif()

# ---- platform GL libs -----------------------------------------------------------
if(WIN32)
    set(LNG_GL_LIBS opengl32)
else()
    find_package(OpenGL REQUIRED)
    set(LNG_GL_LIBS OpenGL::GL ${CMAKE_DL_LIBS})
endif()

# ---- shared, game-agnostic core (C) --------------------------------------------
set(LNG_PLATFORM_SRC ${LNG_DIR}/launcher_platform_sdl2.c)
if(LNG_SDL3)
    set(LNG_PLATFORM_SRC ${LNG_DIR}/launcher_platform_sdl3.c)
endif()

add_library(launcher_ng_core STATIC
    ${LNG_DIR}/launcher_model.c
    ${LNG_PLATFORM_SRC}
    ${LNG_DIR}/launcher_gl.c
    ${LNG_DIR}/launcher_input.c
    ${LNG_DIR}/launcher_files.c
    ${LNG_DIR}/launcher_debug.c
    ${LNG_DIR}/third_party/tinyfiledialogs.c
)
target_include_directories(launcher_ng_core PUBLIC ${LNG_DIR} ${RUNNER_SRC})
target_link_libraries(launcher_ng_core PUBLIC ${LNG_SDL_LIB} ${LNG_GL_LIBS})
if(LNG_SDL3)
    target_compile_definitions(launcher_ng_core PUBLIC LNG_SDL3)
endif()

# ---- executable: Dear ImGui backend (the shipping UI) --------------------------
add_executable(mmx-launcher-imgui
    ${LNG_DIR}/proto_main.c
    ${LNG_DIR}/backends/imgui/launcher_imgui.cpp
)
target_link_libraries(mmx-launcher-imgui PRIVATE
    launcher_ng_core imgui ${LNG_SDL_LIB} ${LNG_GL_LIBS})

# Stage fonts + images next to a target's exe (loaded via SDL_GetBasePath).
function(stage_launcher_assets tgt)
    add_custom_command(TARGET ${tgt} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:${tgt}>/assets/fonts
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:${tgt}>/assets/img
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${LAUNCHER_ASSETS}/fonts/LatoLatin-Regular.ttf
                $<TARGET_FILE_DIR:${tgt}>/assets/fonts/
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${LAUNCHER_ASSETS}/img/brand_mark.tga ${LAUNCHER_ASSETS}/img/snes_pad.tga
                $<TARGET_FILE_DIR:${tgt}>/assets/img/
        VERBATIM)
    if(EXISTS ${MMX_BOXART})
        add_custom_command(TARGET ${tgt} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${MMX_BOXART} $<TARGET_FILE_DIR:${tgt}>/assets/img/boxart.tga
            VERBATIM)
    endif()
endfunction()
stage_launcher_assets(mmx-launcher-imgui)

# ---- Clay reference backend (SDL3 only — it uses SDL3 events directly) ---------
if(LNG_SDL3)
    add_executable(mmx-launcher-clay
        ${LNG_DIR}/proto_main.c
        ${LNG_DIR}/backends/clay/launcher_clay.cpp
        ${LNG_DIR}/backends/clay/clay_impl.c
        ${LNG_DIR}/backends/clay/clay_renderer_gl.c
        ${LNG_DIR}/third_party/gl_core_3_1.c
    )
    target_compile_definitions(mmx-launcher-clay PRIVATE LNG_SDL3)
    target_link_libraries(mmx-launcher-clay PRIVATE launcher_ng_core ${LNG_SDL_LIB} ${LNG_GL_LIBS})
    stage_launcher_assets(mmx-launcher-clay)
endif()
