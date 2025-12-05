idf_component_register(SRCS "config_store.c"
                    INCLUDE_DIRS "."
                    REQUIRES nvs_flash json
                    PRIV_REQUIRES main)

