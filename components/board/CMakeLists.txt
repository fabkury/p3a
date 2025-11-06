idf_component_register(
    SRCS "src/board.c" "src/pins.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_hw_support
    PRIV_REQUIRES waveshare__esp32_p4_wifi6_touch_lcd_4b
)
