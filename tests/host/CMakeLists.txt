# Host unit tests for p3a 2.0 (tier T1 in docs/2.0/TESTING.md).
#
#   cmake -S tests/host -B build-host -G Ninja
#   cmake --build build-host
#   ctest --test-dir build-host --output-on-failure
#
# Sources under test are compiled straight from components/ with the host
# compiler. Only code with no ESP-IDF includes can be listed here; that is
# the point.
cmake_minimum_required(VERSION 3.16)
project(p3a_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

get_filename_component(P3A_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(P3A_COMPONENTS "${P3A_ROOT}/components")

# Sanitizers: on by default where the toolchain has them (Linux, CI). MinGW
# does not ship the runtime libraries.
if(WIN32)
    set(P3A_SANITIZE_DEFAULT OFF)
else()
    set(P3A_SANITIZE_DEFAULT ON)
endif()
option(P3A_SANITIZE "Build tests with AddressSanitizer and UBSan" ${P3A_SANITIZE_DEFAULT})

add_library(unity STATIC third_party/unity/unity.c)
target_include_directories(unity PUBLIC third_party/unity)

enable_testing()

# p3a_host_test(<name> SOURCES <files...> [INCLUDES <dirs...>])
# One executable and one CTest entry per test file. Paths in SOURCES are
# relative to tests/host or absolute; component sources are given through
# ${P3A_COMPONENTS}.
function(p3a_host_test name)
    cmake_parse_arguments(ARG "" "" "SOURCES;INCLUDES" ${ARGN})
    add_executable(${name} ${ARG_SOURCES})
    target_include_directories(${name} PRIVATE ${ARG_INCLUDES})
    target_link_libraries(${name} PRIVATE unity)
    target_compile_options(${name} PRIVATE -Wall -Wextra -Werror -Wshadow -Wconversion -g)
    if(P3A_SANITIZE)
        target_compile_options(${name} PRIVATE
            -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
        target_link_options(${name} PRIVATE -fsanitize=address,undefined)
    endif()
    add_test(NAME ${name} COMMAND ${name})
endfunction()

# Stand-ins for the few IDF headers that host-testable code includes.
set(P3A_STUBS "${CMAKE_CURRENT_SOURCE_DIR}/stubs")

# --- util -------------------------------------------------------------------
set(UTIL "${P3A_COMPONENTS}/p3a_util")
p3a_host_test(test_version
    SOURCES util/test_version.c ${UTIL}/p3a_version.c
    INCLUDES ${UTIL}/include)
p3a_host_test(test_err
    SOURCES util/test_err.c ${UTIL}/p3a_err.c
    INCLUDES ${UTIL}/include ${P3A_STUBS})
p3a_host_test(test_task_policy
    SOURCES util/test_task_policy.c ${UTIL}/p3a_task_policy.c
    INCLUDES ${UTIL}/include)
p3a_host_test(test_job_table
    SOURCES util/test_job_table.c ${UTIL}/p3a_job_table.c
    INCLUDES ${UTIL}/include ${P3A_STUBS})
p3a_host_test(test_event_table
    SOURCES util/test_event_table.c ${UTIL}/p3a_event_table.c
    INCLUDES ${UTIL}/include ${P3A_STUBS})
p3a_host_test(test_mem_pressure
    SOURCES util/test_mem_pressure.c ${UTIL}/p3a_mem_pressure.c
    INCLUDES ${UTIL}/include)

# --- storage ----------------------------------------------------------------
set(STORAGE "${P3A_COMPONENTS}/p3a_storage")
p3a_host_test(test_sd_path
    SOURCES storage/test_sd_path.c ${STORAGE}/sd_path.c
    INCLUDES ${STORAGE}/include ${P3A_STUBS})
p3a_host_test(test_frame
    SOURCES storage/test_frame.c ${STORAGE}/p3a_frame.c ${UTIL}/p3a_crc32.c ${UTIL}/p3a_err.c
    INCLUDES ${STORAGE}/include ${UTIL}/include ${P3A_STUBS})
