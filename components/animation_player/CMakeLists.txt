# Layer: player. Turns one artwork file into frames on the panel: reads it,
# opens its decoder, fits it to the panel, and feeds the display pipeline as
# its artwork source. What to play and when is decided above it.
idf_component_register(
    SRCS "animation_player.c"
         "player_clip.c"
         "player_file.c"
         "player_fit.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES animation_decoder display_renderer frame_trace p3a_board p3a_util
)
