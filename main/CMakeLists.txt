idf_component_register(
    SRCS "p3a_main.c"
    PRIV_REQUIRES esp_app_format p3a_board p3a_storage p3a_util sdio_bus slave_ota p3a_test_hooks
)
