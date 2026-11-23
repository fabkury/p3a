# Layer: ui. What is on the screen besides the artwork: one model of the
# screens and notices that are up (ui_model.c), the notices about playback
# worked out from the scheduler's state (ui_player.c), and their drawing
# (ui_canvas.c, ui_font.c, ui_draw.c), and fingers turned into gestures
# (ui_gesture.c) and what each gesture does on each screen (ui_route.c).
# All of those are pure and also
# compiled by the host tests; p3a_ui.c, ui_display.c and ui_touch.c are the
# wiring.
#
# fonts/ holds the built-in font, made by tools/gen_font.py and committed.
idf_component_register(
    SRCS "p3a_ui.c"
         "ui_canvas.c"
         "ui_display.c"
         "ui_draw.c"
         "ui_font.c"
         "ui_gesture.c"
         "ui_model.c"
         "ui_player.c"
         "ui_route.c"
         "ui_touch.c"
    INCLUDE_DIRS "include"
    REQUIRES p3a_util
    PRIV_REQUIRES display_renderer esp_timer espressif__esp_lcd_touch p3a_board p3a_settings p3a_storage play_scheduler
    EMBED_FILES "fonts/ui.p3afont"
)
