idf_component_register(
    SRCS "sdcard_channel.c"
         "sdcard_channel_impl.c"
         "makapix_channel_impl.c"
         "makapix_channel_utils.c"
         "makapix_channel_refresh.c"
         "makapix_channel_events.c"
         "vault_storage.c"
         "animation_metadata.c"
         "playlist_manager.c"
         "download_manager.c"
         "channel_settings.c"
         "channel_cache.c"
         "load_tracker.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common json makapix sdio_bus config_store sync_playlist wifi_manager live_mode p3a_core play_scheduler event_bus
)
