idf_component_register(
    SRCS
        "src/ui_mode_switch.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        freertos
        esp_timer
        esp_lcd_touch
)

