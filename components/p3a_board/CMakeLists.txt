# Layer: board. The Waveshare ESP32-P4-WIFI6-Touch-LCD-4B: panel,
# framebuffers, backlight, BOOT button, touch controller, SD card. The
# co-processor link joins in the last step of M1.2.
idf_component_register(
    SRCS "p3a_board_button.c" "p3a_board_display.c" "p3a_board_ff_alloc.c"
         "p3a_board_sd.c" "p3a_board_touch.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_lcd espressif__esp_lcd_touch sdmmc
    PRIV_REQUIRES esp_driver_gpio waveshare__esp32_p4_wifi6_touch_lcd_4b p3a_util sd_idle_wait
)

# FatFS takes its buffers from p3a_board_ff_alloc.c (cache-line aligned, so the
# SD driver can DMA them directly). --wrap redirects the calls; -u keeps the
# object in the link even when this library is scanned before FatFS asks.
target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=ff_memalloc" "-Wl,--wrap=ff_memfree"
    "-Wl,-u,__wrap_ff_memalloc" "-Wl,-u,__wrap_ff_memfree")
