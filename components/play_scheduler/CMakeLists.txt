idf_component_register(
    SRCS "play_scheduler.c"
         "play_scheduler_buffers.c"
         "play_scheduler_pick.c"
         "play_scheduler_swrr.c"
         "play_scheduler_nae.c"
         "play_scheduler_timer.c"
         "play_scheduler_cache.c"
         "play_scheduler_refresh.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common channel_manager config_store p3a_core freertos esp_timer makapix
)
