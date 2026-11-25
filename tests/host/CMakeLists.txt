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

# cJSON, the same release the IDF ships (1.7.19), for code that reads and
# writes JSON. Third-party code: built without the project's warning flags.
add_library(cjson STATIC third_party/cJSON/cJSON.c)
target_include_directories(cjson PUBLIC third_party/cJSON)

add_library(unity STATIC third_party/unity/unity.c)
target_include_directories(unity PUBLIC third_party/unity)

enable_testing()

# p3a_host_test(<name> SOURCES <files...> [INCLUDES <dirs...>] [LIBS <libraries...>])
# One executable and one CTest entry per test file. Paths in SOURCES are
# relative to tests/host or absolute; component sources are given through
# ${P3A_COMPONENTS}.
function(p3a_host_test name)
    cmake_parse_arguments(ARG "" "" "SOURCES;INCLUDES;LIBS" ${ARGN})
    add_executable(${name} ${ARG_SOURCES})
    target_include_directories(${name} PRIVATE ${ARG_INCLUDES})
    target_link_libraries(${name} PRIVATE unity ${ARG_LIBS})
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
p3a_host_test(test_clock
    SOURCES util/test_clock.c ${UTIL}/p3a_clock.c
    INCLUDES ${UTIL}/include ${P3A_COMPONENTS}/p3a_net/include ${P3A_STUBS})
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

# --- settings ---------------------------------------------------------------
# The table and the typed struct are generated from the real schema, exactly
# as the firmware build does it.
set(SETTINGS "${P3A_COMPONENTS}/p3a_settings")
set(SETTINGS_GEN "${CMAKE_BINARY_DIR}/gen")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(
    COMMAND ${Python3_EXECUTABLE} "${P3A_ROOT}/tools/gen_settings.py" "${SETTINGS}/settings.json" "${SETTINGS_GEN}"
    RESULT_VARIABLE settings_gen_result)
if(NOT settings_gen_result EQUAL 0)
    message(FATAL_ERROR "settings.json was rejected; see the message above")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${SETTINGS}/settings.json" "${P3A_ROOT}/tools/gen_settings.py")
p3a_host_test(test_settings_model
    SOURCES settings/test_settings_model.c ${SETTINGS}/p3a_settings_model.c ${SETTINGS_GEN}/p3a_settings_gen.c
            ${STORAGE}/sd_path.c
    INCLUDES ${SETTINGS}/include ${SETTINGS_GEN} ${STORAGE}/include ${P3A_STUBS}
    LIBS cjson)

# --- net --------------------------------------------------------------------
set(NET "${P3A_COMPONENTS}/p3a_net")
p3a_host_test(test_http
    SOURCES net/test_http.c ${NET}/http_route.c ${NET}/http_envelope.c ${UTIL}/p3a_crc32.c ${UTIL}/p3a_err.c
    INCLUDES ${NET}/include ${UTIL}/include ${P3A_STUBS}
    LIBS cjson)

# The built-in pages, embedded exactly as the firmware build embeds them.
set(STATIC_GEN "${CMAKE_BINARY_DIR}/gen-static")
execute_process(
    COMMAND ${Python3_EXECUTABLE} "${P3A_ROOT}/tools/embed_static.py" "${NET}/static" "${STATIC_GEN}"
    RESULT_VARIABLE static_gen_result)
if(NOT static_gen_result EQUAL 0)
    message(FATAL_ERROR "the static pages could not be embedded; see the message above")
endif()
file(GLOB_RECURSE static_sources CONFIGURE_DEPENDS "${NET}/static/*")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${static_sources} "${P3A_ROOT}/tools/embed_static.py")
p3a_host_test(test_setup
    SOURCES net/test_setup.c ${NET}/net_dns.c ${NET}/http_static.c ${STATIC_GEN}/p3a_static_gen.c
    INCLUDES ${NET}/include)

# --- player -----------------------------------------------------------------
# The BMP decoder is p3a's own parser of untrusted bytes: run on the corpus
# and on mutated copies of it, where the sanitizers can see every access.
set(DECODER "${P3A_COMPONENTS}/animation_decoder")
p3a_host_test(test_bmp_decoder
    SOURCES player/test_bmp_decoder.c ${DECODER}/bmp_animation_decoder.c
    # The stubs first: they stand in for psram_alloc.h; p3a_err.h is the real one.
    INCLUDES ${DECODER}/include ${DECODER} ${P3A_STUBS} ${UTIL}/include)
target_compile_definitions(test_bmp_decoder PRIVATE
    P3A_DECODER_CORPUS="${P3A_ROOT}/tests/assets/decoders")
# The decoder came from 1.x and was not written to -Wconversion. The
# sanitizers are the point of this test, so only that warning is relaxed.
target_compile_options(test_bmp_decoder PRIVATE -Wno-conversion -Wno-sign-conversion)
p3a_host_test(test_player_fit
    SOURCES player/test_player_fit.c ${P3A_COMPONENTS}/animation_player/player_fit.c
    INCLUDES ${P3A_COMPONENTS}/animation_player)

# --- library ----------------------------------------------------------------
set(LIBRARY "${P3A_COMPONENTS}/p3a_library")
p3a_host_test(test_record_index
    SOURCES library/test_record_index.c ${LIBRARY}/p3a_record.c ${LIBRARY}/p3a_index.c ${LIBRARY}/p3a_source.c
            ${P3A_COMPONENTS}/source_local/source_local_names.c ${UTIL}/p3a_err.c
    INCLUDES ${LIBRARY}/include ${P3A_COMPONENTS}/source_local/include ${P3A_STUBS} ${UTIL}/include)
p3a_host_test(test_evict
    SOURCES library/test_evict.c ${LIBRARY}/p3a_evict.c
    INCLUDES ${LIBRARY}/include ${P3A_STUBS})

# --- sources ----------------------------------------------------------------
p3a_host_test(test_giphy_parse
    SOURCES sources/test_giphy_parse.c ${P3A_COMPONENTS}/source_giphy/giphy_parse.c ${LIBRARY}/p3a_record.c
            ${UTIL}/p3a_err.c
    INCLUDES ${P3A_COMPONENTS}/source_giphy ${LIBRARY}/include ${UTIL}/include ${P3A_STUBS}
    LIBS cjson)
target_compile_definitions(test_giphy_parse PRIVATE P3A_SOURCE_ASSETS="${P3A_ROOT}/tests/assets/sources")
p3a_host_test(test_klipy_parse
    SOURCES sources/test_klipy_parse.c ${P3A_COMPONENTS}/source_klipy/klipy_parse.c ${LIBRARY}/p3a_record.c
            ${UTIL}/p3a_err.c
    INCLUDES ${P3A_COMPONENTS}/source_klipy ${LIBRARY}/include ${UTIL}/include ${P3A_STUBS}
    LIBS cjson)
target_compile_definitions(test_klipy_parse PRIVATE P3A_SOURCE_ASSETS="${P3A_ROOT}/tests/assets/sources")

# --- scheduler --------------------------------------------------------------
set(SCHEDULER "${P3A_COMPONENTS}/play_scheduler")
p3a_host_test(test_sched_core
    SOURCES player/test_sched_core.c ${SCHEDULER}/sched_core.c
    INCLUDES ${SCHEDULER})
p3a_host_test(test_playset
    SOURCES player/test_playset.c ${SCHEDULER}/playset.c ${UTIL}/p3a_err.c
    INCLUDES ${SCHEDULER}/include ${P3A_STUBS} ${UTIL}/include
    LIBS cjson)
p3a_host_test(test_sched_refresh
    SOURCES player/test_sched_refresh.c ${SCHEDULER}/sched_refresh.c
    INCLUDES ${SCHEDULER})
p3a_host_test(test_sched_turn
    SOURCES player/test_sched_turn.c ${SCHEDULER}/sched_turn.c
    INCLUDES ${SCHEDULER})
p3a_host_test(test_sched_pick
    SOURCES player/test_sched_pick.c ${SCHEDULER}/sched_pick.c ${SCHEDULER}/sched_core.c
    INCLUDES ${SCHEDULER})
p3a_host_test(test_sched_history
    SOURCES player/test_sched_history.c ${SCHEDULER}/sched_history.c ${LIBRARY}/p3a_record.c ${UTIL}/p3a_err.c
    INCLUDES ${SCHEDULER} ${LIBRARY}/include ${P3A_STUBS} ${UTIL}/include)

# --- ui ---------------------------------------------------------------------
set(UI "${P3A_COMPONENTS}/p3a_ui")
p3a_host_test(test_ui_model
    SOURCES ui/test_ui_model.c ${UI}/ui_model.c ${UTIL}/p3a_ui_hooks.c
    INCLUDES ${UI} ${UTIL}/include)
p3a_host_test(test_ui_player
    SOURCES ui/test_ui_player.c ${UI}/ui_player.c ${UTIL}/p3a_ui_hooks.c ${UTIL}/p3a_err.c
    INCLUDES ${UI} ${UTIL}/include ${P3A_STUBS})
p3a_host_test(test_ui_text
    SOURCES ui/test_ui_text.c ${UI}/ui_canvas.c ${UI}/ui_font.c
    INCLUDES ${UI})
p3a_host_test(test_ui_gesture
    SOURCES ui/test_ui_gesture.c ${UI}/ui_gesture.c
    INCLUDES ${UI}
    LIBS m)
p3a_host_test(test_ui_card
    SOURCES ui/test_ui_card.c ${UI}/ui_card.c ${UTIL}/p3a_ui_hooks.c
    INCLUDES ${UI} ${UTIL}/include)
p3a_host_test(test_usb_session
    SOURCES ui/test_usb_session.c ${P3A_COMPONENTS}/p3a_usb/usb_session.c
    INCLUDES ${P3A_COMPONENTS}/p3a_usb)
p3a_host_test(test_ui_route
    SOURCES ui/test_ui_route.c ${UI}/ui_route.c
    INCLUDES ${UI} ${UTIL}/include)
p3a_host_test(test_ui_image
    SOURCES ui/test_ui_image.c ${UI}/ui_image.c ${UI}/ui_canvas.c
    INCLUDES ${UI})
p3a_host_test(test_ui_credit
    SOURCES ui/test_ui_credit.c ${UI}/ui_credit.c
    INCLUDES ${UI})
p3a_host_test(test_ui_clock
    SOURCES ui/test_ui_clock.c ${UI}/ui_clock.c ${UI}/ui_canvas.c ${UI}/ui_font.c
    INCLUDES ${UI})
target_compile_definitions(test_ui_clock PRIVATE UI_FONT_FILE="${UI}/fonts/ui.p3afont")
p3a_host_test(test_ui_draw
    SOURCES ui/test_ui_draw.c ${UI}/ui_draw.c ${UI}/ui_canvas.c ${UI}/ui_font.c ${UI}/ui_image.c ${UI}/ui_clock.c ${UTIL}/p3a_ui_hooks.c
    INCLUDES ${UI} ${UTIL}/include)
target_compile_definitions(test_ui_draw PRIVATE UI_FONT_FILE="${UI}/fonts/ui.p3afont")

# Not a test: draws the UI's screens into a file on the workstation, with
# the code the device runs. The device tests compare the panel with it
# (tests/device/test_ui_drawing.py), and it is handy for looking at a change.
add_executable(ui_preview ui/ui_preview.c ${UI}/ui_draw.c ${UI}/ui_canvas.c ${UI}/ui_font.c ${UI}/ui_image.c ${UI}/ui_clock.c ${UTIL}/p3a_ui_hooks.c)
target_include_directories(ui_preview PRIVATE ${UI} ${UTIL}/include)
target_compile_options(ui_preview PRIVATE -Wall -Wextra -Werror -Wshadow -Wconversion -g)
