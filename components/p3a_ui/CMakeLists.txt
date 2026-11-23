# Layer: ui. What is on the screen besides the artwork: one model of the
# screens and notices that are up (ui_model.c), and the notices about
# playback worked out from the scheduler's state (ui_player.c). Both are
# pure and also compiled by the host tests.
idf_component_register(
    SRCS "p3a_ui.c"
         "ui_model.c"
         "ui_player.c"
    INCLUDE_DIRS "include"
    REQUIRES p3a_util
    PRIV_REQUIRES esp_timer p3a_storage play_scheduler
)
