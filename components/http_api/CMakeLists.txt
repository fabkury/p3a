set(http_api_requires
    esp_http_server mdns json config_store freertos esp_wifi esp_timer esp_netif p3a_core
    makapix ota_manager p3a_core channel_manager play_scheduler event_bus
)

if(CONFIG_OTA_DEV_MODE)
    list(APPEND http_api_requires live_mode)
endif()

idf_component_register(SRCS "http_api.c"
                             "http_api_utils.c"
                             "http_api_rest.c"
                             "http_api_pages.c"
                             "http_api_pico8.c"
                             "http_api_ota.c"
                             "http_api_upload.c"
                    INCLUDE_DIRS "."
                    REQUIRES ${http_api_requires}
                    PRIV_REQUIRES main)
