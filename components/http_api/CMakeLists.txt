set(http_api_requires
    esp_http_server mdns json config_store freertos esp_wifi esp_timer esp_netif p3a_core
    makapix ota_manager p3a_core channel_manager play_scheduler event_bus show_url
    storage_eviction giphy art_institution pin_lists slave_ota
)

idf_component_register(SRCS "http_api.c"
                             "http_api_utils.c"
                             "http_api_rest_status.c"
                             "http_api_rest_settings.c"
                             "http_api_rest_actions.c"
                             "http_api_rest_playsets.c"
                             "http_api_rest_museum.c"
                             "http_api_rest_pinned.c"
                             "http_api_pages.c"
                             "http_api_pico8.c"
                             "http_api_ota.c"
                             "http_api_upload.c"
                    INCLUDE_DIRS "."
                    REQUIRES ${http_api_requires}
                    PRIV_REQUIRES main)

# Version/API defs consumed by http_api_rest_status.c (/status endpoint).
# These are injected PRIVATE per-component and do NOT propagate via REQUIRES,
# so http_api must declare its own — otherwise version.h falls back to
# FW_VERSION "0.0.0" and the api_version field falls back to 1 (real values:
# PROJECT_VER and P3A_API_VERSION). Mirrors main/CMakeLists.txt.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    FW_VERSION_STRING="${PROJECT_VER}"
    FW_VERSION_MAJOR=${FW_VERSION_MAJOR}
    FW_VERSION_MINOR=${FW_VERSION_MINOR}
    FW_VERSION_PATCH=${FW_VERSION_PATCH}
    P3A_API_VERSION=${P3A_API_VERSION}
)
