idf_component_register(
    SRCS "src/video_player.c"
    INCLUDE_DIRS "include"
    REQUIRES storage libwebp_decoder esp_timer esp_lcd p3a_hal animatedgif
    PRIV_REQUIRES esp_lvgl_port waveshare__esp32_p4_wifi6_touch_lcd_4b esp_mm
)

