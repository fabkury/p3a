idf_component_register(
    SRCS "p3a_main.c"
    PRIV_REQUIRES animation_decoder animation_player display_renderer frame_trace esp_app_format p3a_board p3a_net p3a_settings p3a_storage p3a_util source_local sdio_bus slave_ota p3a_test_hooks
)
