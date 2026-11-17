# Layer: board. The Waveshare ESP32-P4-WIFI6-Touch-LCD-4B: panel,
# framebuffers, backlight, BOOT button. Touch, the SD card and the
# co-processor link join in later steps of M1.2.
idf_component_register(
    SRCS "p3a_board_button.c" "p3a_board_display.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_lcd
    PRIV_REQUIRES esp_driver_gpio waveshare__esp32_p4_wifi6_touch_lcd_4b
)
