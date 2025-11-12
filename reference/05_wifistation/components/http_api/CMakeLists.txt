idf_component_register(SRCS "http_api.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_http_server mdns json app_state config_store freertos esp_wifi esp_timer)

