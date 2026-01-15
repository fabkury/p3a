idf_component_register(
    SRCS "playback_queue.c"
    INCLUDE_DIRS "include"
    REQUIRES play_scheduler config_store
)
