idf_component_register(SRCS "config_store.c"
                             "config_store_settings.c"
                             "config_store_giphy.c"
                    INCLUDE_DIRS "."
                    REQUIRES nvs_flash json)

