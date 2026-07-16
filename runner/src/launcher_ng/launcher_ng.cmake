# launcher_ng.cmake — reusable in-game Dear ImGui launcher integration.
#
# One call wires the whole GUI pre-boot launcher into a game target:
#
#     include(${SNESRECOMP_ROOT}/runner/src/launcher_ng/launcher_ng.cmake)
#     snesrecomp_target_launcher_ng(<game_target> [BOXART <path-to-boxart.tga>])
#
# It mirrors the MSVC mmx.vcxproj launcher_ng source list EXACTLY (minus
# proto_main.c, which is the standalone harness main, and minus crc32/sha256/
# keybinds, which every game already compiles from the runner sources). It uses
# the VENDORED ImGui at launcher_ng/third_party/imgui — no network / FetchContent,
# so mingw-on-Windows and Linux/AppImage builds are both offline and DRY.
#
# The launcher entry snes_launcher_run_window() is guarded by SNES_LAUNCHER in
# main.c; this module defines it and links Dear ImGui + SDL2 + OpenGL. Assets
# (fonts + brand/pad images + optional per-game box art) are staged next to the
# exe post-build, matching the runtime SDL_GetBasePath() lookup.

set(LNG_ROOT  ${CMAKE_CURRENT_LIST_DIR})
set(LNG_IMGUI ${LNG_ROOT}/third_party/imgui)
set(LNG_ASSETS ${LNG_ROOT}/../launcher/assets)

# The ImGui backend is C++; the game project() is often C-only. enable_language
# must run at directory scope (not inside the function, which executes during
# generation), so it lives here — safe/idempotent if CXX is already enabled.
enable_language(CXX)

function(snesrecomp_target_launcher_ng TGT)
    cmake_parse_arguments(LNG "" "BOXART" "" ${ARGN})

    set_target_properties(${TGT} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

    target_sources(${TGT} PRIVATE
        # game-agnostic launcher core (C)
        ${LNG_ROOT}/launcher_model.c
        ${LNG_ROOT}/launcher_platform_sdl2.c
        ${LNG_ROOT}/launcher_gl.c
        ${LNG_ROOT}/launcher_input.c
        ${LNG_ROOT}/launcher_files.c
        ${LNG_ROOT}/launcher_debug.c
        ${LNG_ROOT}/launcher_binds.c
        ${LNG_ROOT}/launcher_ng_capi.c          # implements snes_launcher_run_window()
        ${LNG_ROOT}/third_party/tinyfiledialogs.c
        # Dear ImGui backend (the shipping UI) + vendored ImGui (C++)
        ${LNG_ROOT}/backends/imgui/launcher_imgui.cpp
        ${LNG_IMGUI}/imgui.cpp
        ${LNG_IMGUI}/imgui_draw.cpp
        ${LNG_IMGUI}/imgui_tables.cpp
        ${LNG_IMGUI}/imgui_widgets.cpp
        ${LNG_IMGUI}/backends/imgui_impl_sdl2.cpp
        ${LNG_IMGUI}/backends/imgui_impl_opengl3.cpp
    )

    target_include_directories(${TGT} PRIVATE
        ${LNG_ROOT}/../launcher      # launcher_capi.h (the snes_launcher_run_window ABI)
        ${LNG_ROOT}                  # launcher_ng headers + relative third_party/stb_*.h
        ${LNG_IMGUI}
        ${LNG_IMGUI}/backends
    )

    target_compile_definitions(${TGT} PRIVATE
        SNES_LAUNCHER            # un-gate the GUI launcher block in main.c
        SDL_MAIN_HANDLED)        # our real main() is the entry point (no SDL_main redirect)

    if(NOT MSVC)
        # the vendored ImGui + tinyfiledialogs compile clean; nothing extra needed.
        # (game recomp warnings are already silenced by the per-target -w.)
    endif()

    # ---- stage runtime assets next to the exe -----------------------------------
    add_custom_command(TARGET ${TGT} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:${TGT}>/assets/fonts
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:${TGT}>/assets/img
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${LNG_ASSETS}/fonts/LatoLatin-Regular.ttf
                ${LNG_ASSETS}/fonts/LatoLatin-Bold.ttf
                $<TARGET_FILE_DIR:${TGT}>/assets/fonts/
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${LNG_ASSETS}/img/brand_mark.tga ${LNG_ASSETS}/img/snes_pad.tga
                $<TARGET_FILE_DIR:${TGT}>/assets/img/
        VERBATIM)
    if(LNG_BOXART AND EXISTS ${LNG_BOXART})
        add_custom_command(TARGET ${TGT} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${LNG_BOXART} $<TARGET_FILE_DIR:${TGT}>/assets/img/boxart.tga
            VERBATIM)
    endif()
endfunction()
