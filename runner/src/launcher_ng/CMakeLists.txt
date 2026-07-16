# launcher_ng — standalone prototype build for the RmlUi-replacement bake-off.
#
# Produces one executable per render backend, both driving the same shared core:
#   mmx-launcher-imgui   (Dear ImGui, MIT)
#   mmx-launcher-clay    (Clay, zlib)      [added when the Clay backend lands]
#
# Deliberately self-contained (its own project + FetchContent deps) so it never
# touches the shipping MMX build. SDL3 keeps the launcher isolated from the
# game's SDL2.
#
# Build (mingw + ninja):
#   cmake -G Ninja -S runner/src/launcher_ng -B runner/src/launcher_ng/build
#   cmake --build runner/src/launcher_ng/build

cmake_minimum_required(VERSION 3.24)
project(launcher_ng C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)   # Clay's C++ macro path requires C++20
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD_REQUIRED ON)

include(FetchContent)

# ---- SDL3 (zlib) : window / GL / input / high-DPI substrate --------------------
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON  CACHE BOOL "" FORCE)
set(SDL_TEST   OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL
    GIT_TAG        release-3.4.12
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(SDL3)

# ---- Dear ImGui (MIT) : has no CMakeLists, so we build our own static lib -------
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui
    GIT_TAG        v1.91.9b
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui PUBLIC SDL3::SDL3)

# ---- platform GL libs -----------------------------------------------------------
if(WIN32)
    set(LNG_GL_LIBS opengl32)
else()
    find_package(OpenGL REQUIRED)
    set(LNG_GL_LIBS OpenGL::GL ${CMAKE_DL_LIBS})
endif()

# ---- shared, game-agnostic core (C) --------------------------------------------
# Include dirs: runner/src (for "launcher/launcher_capi.h") + this dir.
set(LNG_DIR       ${CMAKE_CURRENT_SOURCE_DIR})
set(RUNNER_SRC    ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(LAUNCHER_ASSETS ${RUNNER_SRC}/launcher/assets)
set(MMX_BOXART    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../MegamanXRecomp/recomp/launcher/boxart.tga)

add_library(launcher_ng_core STATIC
    ${LNG_DIR}/launcher_model.c
    ${LNG_DIR}/launcher_platform_sdl3.c
    ${LNG_DIR}/launcher_gl.c
    ${LNG_DIR}/launcher_input.c
    ${LNG_DIR}/launcher_files.c
    ${LNG_DIR}/launcher_debug.c
)
target_include_directories(launcher_ng_core PUBLIC ${LNG_DIR} ${RUNNER_SRC})
target_link_libraries(launcher_ng_core PUBLIC SDL3::SDL3 ${LNG_GL_LIBS})

# ---- executable: Dear ImGui backend --------------------------------------------
add_executable(mmx-launcher-imgui
    ${LNG_DIR}/proto_main.c
    ${LNG_DIR}/backends/imgui/launcher_imgui.cpp
)
target_link_libraries(mmx-launcher-imgui PRIVATE
    launcher_ng_core imgui SDL3::SDL3 ${LNG_GL_LIBS}
)

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

# ---- executable: Clay backend --------------------------------------------------
add_executable(mmx-launcher-clay
    ${LNG_DIR}/proto_main.c
    ${LNG_DIR}/backends/clay/launcher_clay.cpp
    ${LNG_DIR}/backends/clay/clay_impl.c
    ${LNG_DIR}/backends/clay/clay_renderer_gl.c
    ${LNG_DIR}/third_party/gl_core_3_1.c
)
target_link_libraries(mmx-launcher-clay PRIVATE launcher_ng_core SDL3::SDL3 ${LNG_GL_LIBS})
stage_launcher_assets(mmx-launcher-clay)
