# Layer: player. The display pipeline: framebuffer ownership, the producer
# and consumer tasks, vsync pacing, the two upscalers and the FPS overlay.
# What draws the frames (the player, the UI) registers with it from above.
idf_component_register(
    SRCS "display_fps_overlay.c"
         "display_ppa_upscaler.c"
         "display_renderer.c"
         "display_upscaler.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_driver_ppa esp_lcd esp_mm esp_timer frame_trace p3a_board p3a_settings p3a_util
)
