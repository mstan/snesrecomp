cmake_minimum_required(VERSION 3.16)
project(snesref LANGUAGES CXX)

add_executable(snesref frontend.cpp)
target_compile_features(snesref PRIVATE cxx_std_17)

# Homebrew, vcpkg, and current SDL2 development packages normally provide the
# imported target. Fall back to pkg-config for distributions that provide only
# sdl2.pc.
find_package(SDL2 CONFIG QUIET)
if(TARGET SDL2::SDL2)
    target_link_libraries(snesref PRIVATE SDL2::SDL2)
elseif(TARGET SDL2::SDL2-static)
    target_link_libraries(snesref PRIVATE SDL2::SDL2-static)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
    target_link_libraries(snesref PRIVATE PkgConfig::SDL2)
endif()

if(CMAKE_DL_LIBS)
    target_link_libraries(snesref PRIVATE ${CMAKE_DL_LIBS})
endif()

if(MSVC)
    target_compile_options(snesref PRIVATE /W4)
else()
    target_compile_options(snesref PRIVATE -Wall -Wextra -Wpedantic)
endif()
