idf_component_register(
    SRCS
        "src/graphics_handoff.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        freertos
        esp_lcd
        esp_lvgl_port
        p3a_hal
        waveshare__esp32_p4_wifi6_touch_lcd_4b
        lvgl
)

