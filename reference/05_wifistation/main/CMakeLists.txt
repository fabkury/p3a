idf_component_register(SRCS "station_example_main.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_wifi nvs_flash esp_http_server app_state http_api)
