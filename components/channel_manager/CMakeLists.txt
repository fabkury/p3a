idf_component_register(
    SRCS "sdcard_channel.c"
         "channel_player.c"
         "sdcard_channel_impl.c"
         "makapix_channel_impl.c"
         "vault_storage.c"
         "animation_metadata.c"
         "playlist_manager.c"
         "play_navigator.c"
         "download_manager.c"
         "channel_settings.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common json makapix sdio_bus config_store sync_playlist wifi_manager
)

