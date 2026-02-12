idf_component_register(
    SRCS "p3a_board_button.c" "p3a_board_display.c"
         "p3a_board_fs.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_lcd littlefs espressif__esp_lcd_touch event_bus
    PRIV_REQUIRES waveshare__esp32_p4_wifi6_touch_lcd_4b nvs_flash
)
