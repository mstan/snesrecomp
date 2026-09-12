# Optional real-ROM integration check; the game and host sources are identical
# to the executable. The test entry includes the host to exercise its adapter.
function(snesrecomp_target_mmx_state_tests target game_main)
    option(MMX_STATE_TESTS "Build ROM-backed MMX save/rewind checks" OFF)
    if(NOT MMX_STATE_TESTS)
        return()
    endif()
    get_target_property(_sources ${target} SOURCES)
    list(REMOVE_ITEM _sources src/main.c "${game_main}"
        "${SNESRECOMP_RUNNER_ROOT}/src/desktop/host_main.c")
    add_executable(mmx_state_tests
        "${SNESRECOMP_RUNNER_ROOT}/tests/mmx_state_runtime.c" ${_sources})
    foreach(_property INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS LINK_LIBRARIES LINK_OPTIONS)
        get_target_property(_value ${target} ${_property})
        if(_value)
            set_property(TARGET mmx_state_tests PROPERTY ${_property} "${_value}")
        endif()
    endforeach()
    target_compile_definitions(mmx_state_tests PRIVATE MMX_GAME_MAIN="${game_main}")
endfunction()
