# Opt-in, independently of the trace debugger. No Lua download in normal builds.
option(SNESRECOMP_ENABLE_LUA "Build the local TCP Lua scripting spike" OFF)
if(SNESRECOMP_ENABLE_LUA)
    include(FetchContent)
    FetchContent_Declare(snes_lua
        URL https://www.lua.org/ftp/lua-5.4.9.tar.gz
        URL_HASH SHA256=2335b6c582a52654f94612bf10d2f4672805d05329aa6568b1d8cd9e5c6fb8e6)
    FetchContent_MakeAvailable(snes_lua)
    file(GLOB _snes_lua_sources "${snes_lua_SOURCE_DIR}/src/*.c")
    list(FILTER _snes_lua_sources EXCLUDE REGEX "/(lua|luac)\\.c$")
    add_library(snesrecomp_lua STATIC ${_snes_lua_sources}
        ${CMAKE_CURRENT_LIST_DIR}/src/lua_bridge.c)
    target_include_directories(snesrecomp_lua PRIVATE "${snes_lua_SOURCE_DIR}/src")
    target_compile_definitions(snesrecomp_lua PUBLIC SNESRECOMP_ENABLE_LUA=1)
    if(WIN32)
        target_link_libraries(snesrecomp_lua PRIVATE ws2_32)
    else()
        target_link_libraries(snesrecomp_lua PRIVATE m)
    endif()
    list(APPEND SNESRECOMP_RUNNER_LIBRARIES snesrecomp_lua)
endif()
