idf_component_register(
    SRCS "src/p3a_hal.c" "src/display.c" "src/touch.c"
    INCLUDE_DIRS "include"
    REQUIRES board esp_lvgl_port waveshare__esp32_p4_wifi6_touch_lcd_4b
)
