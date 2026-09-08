cmake_minimum_required(VERSION 3.16)
project(SnesRecompDiagnosticFlags C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(SNESRECOMP_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." CACHE PATH
    "Path to the snesrecomp checkout under test")
include("${SNESRECOMP_ROOT}/runner/runner.cmake")

add_executable(cmake_diagnostic_gates
    "${SNESRECOMP_ROOT}/tests/runtime_dispatch/diagnostic_gates_test.c"
    "${SNESRECOMP_ROOT}/runner/src/common_cpu_infra.c")
target_include_directories(cmake_diagnostic_gates PRIVATE
    ${SNESRECOMP_RUNNER_INCLUDE_DIRS})
target_compile_options(cmake_diagnostic_gates PRIVATE
    -Wall -Wextra -Werror -ffunction-sections -fdata-sections)
get_source_file_property(_diag_gate_defs
    "${SNESRECOMP_ROOT}/runner/src/common_cpu_infra.c"
    COMPILE_DEFINITIONS)
if(_diag_gate_defs)
    set_source_files_properties(
        "${SNESRECOMP_ROOT}/tests/runtime_dispatch/diagnostic_gates_test.c"
        PROPERTIES COMPILE_DEFINITIONS "${_diag_gate_defs}")
endif()

add_executable(cmake_apu_port_guest_time
    "${SNESRECOMP_ROOT}/tests/runtime_dispatch/apu_port_guest_time_test.c"
    "${SNESRECOMP_ROOT}/runner/src/snes/apu.c"
    "${SNESRECOMP_ROOT}/runner/src/snes/spc.c"
    "${SNESRECOMP_ROOT}/runner/src/snes/dsp.c")
target_include_directories(cmake_apu_port_guest_time PRIVATE
    ${SNESRECOMP_RUNNER_INCLUDE_DIRS})
target_compile_options(cmake_apu_port_guest_time PRIVATE
    -Wall -Wextra -Werror
    -Wno-error=unknown-pragmas -Wno-error=comment
    -ffunction-sections -fdata-sections)
get_source_file_property(_apu_diag_defs
    "${SNESRECOMP_ROOT}/runner/src/snes/apu.c"
    COMPILE_DEFINITIONS)
if(_apu_diag_defs)
    set_source_files_properties(
        "${SNESRECOMP_ROOT}/tests/runtime_dispatch/apu_port_guest_time_test.c"
        PROPERTIES COMPILE_DEFINITIONS "${_apu_diag_defs}")
endif()

if(SNESRECOMP_ENABLE_FUNC_SNAPSHOT OR SNESRECOMP_ENABLE_TRACE OR SNES_COSIM)
    set_property(SOURCE
        "${SNESRECOMP_ROOT}/tests/runtime_dispatch/diagnostic_gates_test.c"
        APPEND PROPERTY COMPILE_DEFINITIONS
        EXPECT_FUNC_SNAPSHOT_ON=1)
endif()
if(SNESRECOMP_ENABLE_STACK_BALANCE_DIAGNOSTICS OR
   SNESRECOMP_ENABLE_TRACE OR
   SNES_COSIM)
    set_property(SOURCE
        "${SNESRECOMP_ROOT}/tests/runtime_dispatch/diagnostic_gates_test.c"
        APPEND PROPERTY COMPILE_DEFINITIONS
        EXPECT_STACK_BALANCE_DIAGNOSTICS_ON=1)
endif()
if(SNESRECOMP_ENABLE_SPC_DIAGNOSTICS OR SNESRECOMP_ENABLE_TRACE OR SNES_COSIM)
    set_property(SOURCE
        "${SNESRECOMP_ROOT}/tests/runtime_dispatch/apu_port_guest_time_test.c"
        APPEND PROPERTY COMPILE_DEFINITIONS
        EXPECT_SPC_DIAGNOSTICS_ON=1)
endif()
