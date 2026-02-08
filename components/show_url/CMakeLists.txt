idf_component_register(
    SRCS "show_url.c"
    INCLUDE_DIRS "include"
    REQUIRES p3a_core play_scheduler makapix channel_manager esp_http_client esp_timer esp-tls
)
