# Layer: net. Wi-Fi through the ESP32-C6 co-processor, the device's name on
# the LAN, the clock, and the HTTP server with the built-in /api/v2 endpoints.
# Also setup: the access point the device opens when it has no network.
#
# net_backoff.h, net_dns.c, http_route.c, http_envelope.c and http_static.c
# have no IDF includes and are also compiled by the host tests.
# The pages in static/ are gzipped into a C table at configure time; adding
# or editing one re-runs the configure step.
set(gen_dir "${CMAKE_CURRENT_BINARY_DIR}/gen")
set(static_dir "${CMAKE_CURRENT_LIST_DIR}/static")
set(embedder "${CMAKE_CURRENT_LIST_DIR}/../../tools/embed_static.py")
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    idf_build_get_property(python PYTHON)
    execute_process(
        COMMAND ${python} "${embedder}" "${static_dir}" "${gen_dir}"
        RESULT_VARIABLE embed_result)
    if(NOT embed_result EQUAL 0)
        message(FATAL_ERROR "the static pages could not be embedded; see the message above")
    endif()
    file(GLOB_RECURSE static_files CONFIGURE_DEPENDS "${static_dir}/*")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${static_files} "${embedder}")
endif()

# The frame trace's read-out exists only in builds that trace.
set(trace_srcs)
if(CONFIG_P3A_FRAME_TRACE)
    set(trace_srcs "net_trace_api.c")
endif()

idf_component_register(
    SRCS "http_envelope.c" "http_route.c" "http_server.c" "http_static.c"
         "${gen_dir}/p3a_static_gen.c"
         "net_api.c"
         "net_player_api.c"
         ${trace_srcs}
         "net_dns.c"
         "net_link_recovery.c"
         "net_setup.c"
         "net_time.c"
         "net_wifi.c"
         "net_wifi_recovery.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server json p3a_util
    PRIV_REQUIRES play_scheduler source_local p3a_library esp_app_format esp_event esp_partition espcoredump p3a_storage esp_netif esp_timer esp_wifi frame_trace lwip nvs_flash p3a_settings p3a_util
                  espressif__esp_hosted espressif__esp_wifi_remote espressif__mdns
)
