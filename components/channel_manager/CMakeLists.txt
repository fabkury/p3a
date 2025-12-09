idf_component_register(
    SRCS "sdcard_channel.c"
         "channel_player.c"
         "sdcard_channel_impl.c"
         "makapix_channel_impl.c"
         "vault_storage.c"
         "animation_metadata.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common json makapix
)

