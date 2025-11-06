idf_component_register(
    SRCS "src/ui.c"
    INCLUDE_DIRS "include"
    REQUIRES board p3a_hal renderer video_player
    PRIV_REQUIRES lvgl__lvgl waveshare__esp32_p4_wifi6_touch_lcd_4b
)
