idf_component_register(
    SRCS "p3a_board_display.c"
         "p3a_board_fs.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_lcd spiffs espressif__esp_lcd_touch
    PRIV_REQUIRES waveshare__esp32_p4_wifi6_touch_lcd_4b
)
