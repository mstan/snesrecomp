cmake_minimum_required(VERSION 3.16)
project(SnesRecompAudioTraceHistoryConfig C)

set(SNESRECOMP_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." CACHE PATH
    "Path to the snesrecomp checkout under test")
if(NOT DEFINED EXPECT_AUDIO_TRACE_HISTORY)
    message(FATAL_ERROR "EXPECT_AUDIO_TRACE_HISTORY must be defined")
endif()

include("${SNESRECOMP_ROOT}/runner/runner.cmake")

get_source_file_property(_audio_trace_defs
    "${SNESRECOMP_ROOT}/runner/src/audio_trace.c"
    COMPILE_DEFINITIONS)
if(NOT _audio_trace_defs)
    message(FATAL_ERROR "audio_trace.c has no COMPILE_DEFINITIONS")
endif()

set(_expected_def "SNESRECOMP_AUDIO_TRACE_HISTORY=${EXPECT_AUDIO_TRACE_HISTORY}")
if(NOT _expected_def IN_LIST _audio_trace_defs)
    message(FATAL_ERROR
        "expected ${_expected_def} in audio_trace.c definitions; got "
        "${_audio_trace_defs}")
endif()

message(STATUS "audio trace history compile definition: ${_expected_def}")
