# Layer: ui. What is on the screen besides the artwork: one model of the
# screens and notices that are up (ui_model.c), the notices about playback
# worked out from the scheduler's state (ui_player.c), and their drawing
# (ui_canvas.c, ui_font.c, ui_draw.c), and fingers turned into gestures
# (ui_gesture.c) and what each gesture does on each screen (ui_route.c).
# All of those are pure and also
# compiled by the host tests; p3a_ui.c, ui_display.c and ui_touch.c are the
# wiring.
#
# intro/ holds the boot animations and the logo, each a pure function of
# (buffer, context, t), also built and checked on the workstation by
# host/intro-anim-lab; ui_intro.c plays one at boot.
#
# fonts/ holds the built-in font, made by tools/gen_font.py and committed.
idf_component_register(
    SRCS "p3a_ui.c"
         "ui_canvas.c"
         "ui_display.c"
         "ui_draw.c"
         "ui_font.c"
         "ui_gesture.c"
         "ui_intro.c"
         "ui_model.c"
         "ui_player.c"
         "ui_route.c"
         "ui_touch.c"
         "intro/ia_assemble.c"
         "intro/ia_blinds_flip.c"
         "intro/ia_bounce_drop.c"
         "intro/ia_channel_merge.c"
         "intro/ia_checker_tiles.c"
         "intro/ia_color_emerge.c"
         "intro/ia_glitch_settle.c"
         "intro/ia_hue_cycle_lock.c"
         "intro/ia_iris_wipe.c"
         "intro/ia_mosaic_shrink.c"
         "intro/ia_pixel_dissolve.c"
         "intro/ia_pixel_rain.c"
         "intro/ia_plasma_dissolve.c"
         "intro/ia_scanline_reveal.c"
         "intro/ia_smoothstep_fade.c"
         "intro/ia_spiral_reveal.c"
         "intro/ia_starburst.c"
         "intro/ia_swirl_in.c"
         "intro/ia_typewriter.c"
         "intro/ia_venetian.c"
         "intro/ia_voronoi_shatter.c"
         "intro/ia_wave_settle.c"
         "intro/intro_anim_common.c"
         "intro/intro_anim_registry.c"
         "intro/p3a_logo.c"
    INCLUDE_DIRS "include"
    REQUIRES p3a_util
    PRIV_REQUIRES display_renderer esp_hw_support esp_timer espressif__esp_lcd_touch p3a_board p3a_settings play_scheduler
    EMBED_FILES "fonts/ui.p3afont"
)
