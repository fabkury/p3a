idf_component_register(
    SRCS
        "klipy_api.c"
        "klipy_cache.c"
        "klipy_refresh.c"
        "klipy_download.c"
    INCLUDE_DIRS "include"
    REQUIRES
        http_fetch
        json
        p3a_core
        config_store
        channel_manager
        play_scheduler
        sdio_bus
        event_bus
    WHOLE_ARCHIVE
)
