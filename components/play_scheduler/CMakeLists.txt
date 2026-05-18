idf_component_register(
    SRCS "play_scheduler.c"
         "play_scheduler_buffers.c"
         "play_scheduler_pick.c"
         "play_scheduler_swrr.c"
         "play_scheduler_nae.c"
         "play_scheduler_timer.c"
         "play_scheduler_cache.c"
         "play_scheduler_refresh.c"
         "play_scheduler_playsets.c"
         "play_scheduler_lai.c"
         "play_scheduler_navigation.c"
         "playset_store.c"
         "playset_json.c"
         "active_playset_store.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common channel_manager config_store p3a_core freertos esp_timer makapix content_cache mbedtls giphy art_institution json pin_lists
    # WHOLE_ARCHIVE is needed because http_api references playset_store_*
    # symbols. ESP-IDF lists libplay_scheduler.a both before and after
    # libhttp_api.a in the final link command, but ld 2.43 still treats the
    # duplicate as a single scan, so without --whole-archive the second pass
    # never picks up playset_store.c.obj. Matches giphy / art_institution /
    # makapix, which mark themselves WHOLE_ARCHIVE for the same reason.
    WHOLE_ARCHIVE
)
