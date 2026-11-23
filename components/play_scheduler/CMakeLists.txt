# Layer: player. Decides what plays next: the playsets, which channel of
# the active one, which of its artworks, and the history for "back".
idf_component_register(
    SRCS "play_scheduler.c"
         "playset.c"
         "playset_store.c"
         "sched_core.c"
         "sched_history.c"
         "sched_pick.c"
         "sched_turn.c"
    INCLUDE_DIRS "include"
    REQUIRES animation_player json p3a_library
    PRIV_REQUIRES display_renderer esp_hw_support esp_timer p3a_board p3a_settings p3a_storage p3a_util
)
