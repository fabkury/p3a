idf_component_register(
    SRCS "live_mode.c" "swap_future.c"
    INCLUDE_DIRS "."
    REQUIRES esp_timer channel_manager
    PRIV_REQUIRES config_store
)
