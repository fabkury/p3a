idf_component_register(SRCS "http_api.c"
                             "http_api_pages.c"
                             "http_api_ota.c"
                             "http_api_upload.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_http_server mdns json app_state config_store freertos esp_wifi esp_timer esp_netif spiffs makapix ota_manager
                    PRIV_REQUIRES main)

