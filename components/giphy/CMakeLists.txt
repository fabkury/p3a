idf_component_register(
    SRCS
        "giphy_api.c"
        "giphy_cache.c"
        "giphy_refresh.c"
        "giphy_download.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_http_client
        json
        p3a_core
        config_store
        channel_manager
        play_scheduler
        sdio_bus
        event_bus
)
