idf_component_register(
    SRCS "src/renderer.c"
    INCLUDE_DIRS "include"
    REQUIRES storage libwebp_decoder esp_timer video_player
    PRIV_REQUIRES lvgl__lvgl waveshare__esp32_p4_wifi6_touch_lcd_4b esp_mm esp_driver_ppa
)
